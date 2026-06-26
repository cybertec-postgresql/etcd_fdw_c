/*-------------------------------------------------------------------------
 *
 * etcd_conn.c
 *	  libcurl transport + connection cache for etcd_fdw.
 *
 * One libcurl easy handle is kept per (server, user) pair and reused with
 * HTTP keep-alive.  Requests are POSTed to the etcd v3 JSON gateway; on a
 * connection-level failure we fail over to the next configured endpoint.
 * When a user mapping carries credentials we authenticate lazily and cache
 * the returned token, re-authenticating once on an HTTP 401.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <curl/curl.h>

#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "utils/hsearch.h"
#include "utils/inval.h"
#include "utils/memutils.h"
#include "utils/syscache.h"

#include "cJSON.h"
#include "etcd_conn.h"
#include "etcd_json.h"

struct EtcdConn
{
	Oid			serverid;
	Oid			userid;
	CURL	   *curl;
	List	   *endpoints;		/* char* base URLs (in cache context) */
	int			cur_endpoint;	/* index of last-good endpoint */
	char	   *token;			/* auth token or NULL (in cache context) */
	bool		invalidated;	/* server/user mapping changed; rebuild on next use */
	/* connection parameters copied into the cache context */
	bool		use_tls;
	char	   *cafile;
	char	   *certfile;
	char	   *keyfile;
	bool		tls_verify;
	long		connect_timeout_ms;
	long		request_timeout_ms;
	long		max_retries;
	long		retry_backoff_ms;
	char	   *username;
	char	   *password;
};

/* hash key for the connection cache */
typedef struct ConnCacheKey
{
	Oid			serverid;
	Oid			userid;
} ConnCacheKey;

typedef struct ConnCacheEntry
{
	ConnCacheKey key;			/* must be first */
	EtcdConn   *conn;
} ConnCacheEntry;

static HTAB *ConnectionHash = NULL;
static MemoryContext EtcdCacheContext = NULL;
static bool curl_initialized = false;

/* ----- libcurl response accumulation ----- */

typedef struct RecvBuf
{
	StringInfoData buf;
} RecvBuf;

static size_t
recv_callback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
	RecvBuf    *rb = (RecvBuf *) userdata;
	size_t		total = size * nmemb;

	appendBinaryStringInfo(&rb->buf, ptr, (int) total);
	return total;
}

/* Free a cached connection (libcurl handle + its cache-context allocations). */
static void
free_conn(EtcdConn *conn)
{
	if (conn == NULL)
		return;
	if (conn->curl)
		curl_easy_cleanup(conn->curl);
	if (conn->endpoints)
		list_free_deep(conn->endpoints);
	if (conn->token)
		pfree(conn->token);
	if (conn->cafile)
		pfree(conn->cafile);
	if (conn->certfile)
		pfree(conn->certfile);
	if (conn->keyfile)
		pfree(conn->keyfile);
	if (conn->username)
		pfree(conn->username);
	if (conn->password)
		pfree(conn->password);
	pfree(conn);
}

/*
 * Mark all cached connections for rebuild.  Used both by the syscache
 * invalidation callbacks (ALTER/DROP of a server or user mapping) and by
 * etcd_fdw_disconnect().  We only set a flag here — we must NOT free a
 * connection that a running statement may still be holding a pointer to.  The
 * actual free + reconnect happens lazily in etcd_conn_get(), at statement
 * start, when nothing references the old connection.  Returns the count marked.
 */
int
etcd_conn_disconnect_all(void)
{
	HASH_SEQ_STATUS seq;
	ConnCacheEntry *entry;
	int			marked = 0;

	if (ConnectionHash == NULL)
		return 0;

	hash_seq_init(&seq, ConnectionHash);
	while ((entry = (ConnCacheEntry *) hash_seq_search(&seq)) != NULL)
	{
		if (entry->conn && !entry->conn->invalidated)
		{
			entry->conn->invalidated = true;
			marked++;
		}
	}
	return marked;
}

static void
etcd_conn_invalidate(Datum arg, int cacheid, uint32 hashvalue)
{
	(void) etcd_conn_disconnect_all();
}

void
etcd_conn_init(void)
{
	static bool callbacks_registered = false;

	if (!curl_initialized)
	{
		if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
			ereport(ERROR,
					(errcode(ERRCODE_INTERNAL_ERROR),
					 errmsg("etcd_fdw: could not initialize libcurl")));
		curl_initialized = true;
	}

	if (EtcdCacheContext == NULL)
		EtcdCacheContext = AllocSetContextCreate(TopMemoryContext,
												 "etcd_fdw connections",
												 ALLOCSET_DEFAULT_SIZES);

	if (!callbacks_registered)
	{
		CacheRegisterSyscacheCallback(FOREIGNSERVEROID, etcd_conn_invalidate, (Datum) 0);
		CacheRegisterSyscacheCallback(USERMAPPINGOID, etcd_conn_invalidate, (Datum) 0);
		callbacks_registered = true;
	}
}

void
etcd_conn_fini(void)
{
	/* curl_global_cleanup is deliberately not called: other code may use it. */
}

/* Copy a List of char* into the cache context. */
static List *
copy_string_list(List *src)
{
	List	   *out = NIL;
	ListCell   *lc;

	foreach(lc, src)
		out = lappend(out, pstrdup((char *) lfirst(lc)));
	return out;
}

static EtcdConn *
make_conn(EtcdFdwOptions *opts)
{
	MemoryContext old = MemoryContextSwitchTo(EtcdCacheContext);
	EtcdConn   *conn = palloc0(sizeof(EtcdConn));

	conn->serverid = opts->serverid;
	conn->userid = opts->userid;
	conn->endpoints = copy_string_list(opts->endpoints);
	conn->cur_endpoint = 0;
	conn->token = NULL;
	conn->use_tls = opts->use_tls;
	conn->cafile = opts->cafile ? pstrdup(opts->cafile) : NULL;
	conn->certfile = opts->certfile ? pstrdup(opts->certfile) : NULL;
	conn->keyfile = opts->keyfile ? pstrdup(opts->keyfile) : NULL;
	conn->tls_verify = opts->tls_verify;
	conn->connect_timeout_ms = opts->connect_timeout_ms;
	conn->request_timeout_ms = opts->request_timeout_ms;
	conn->max_retries = opts->max_retries;
	conn->retry_backoff_ms = opts->retry_backoff_ms;
	conn->username = opts->username ? pstrdup(opts->username) : NULL;
	conn->password = opts->password ? pstrdup(opts->password) : NULL;

	conn->curl = curl_easy_init();
	if (conn->curl == NULL)
	{
		/*
		 * EtcdCacheContext is long-lived and never reset, so the strings we
		 * just copied into it would leak for the life of the backend.  Free the
		 * half-built connection before raising (free_conn tolerates the NULL
		 * curl handle).
		 */
		MemoryContextSwitchTo(old);
		free_conn(conn);
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("etcd_fdw: could not create libcurl handle")));
	}

	MemoryContextSwitchTo(old);
	return conn;
}

EtcdConn *
etcd_conn_get(EtcdFdwOptions *opts)
{
	ConnCacheKey key;
	ConnCacheEntry *entry;
	bool		found;

	etcd_conn_init();

	if (ConnectionHash == NULL)
	{
		HASHCTL		ctl;

		MemSet(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(ConnCacheKey);
		ctl.entrysize = sizeof(ConnCacheEntry);
		ctl.hcxt = EtcdCacheContext;
		ConnectionHash = hash_create("etcd_fdw connection cache", 8, &ctl,
									 HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}

	MemSet(&key, 0, sizeof(key));
	key.serverid = opts->serverid;
	key.userid = opts->userid;

	entry = hash_search(ConnectionHash, &key, HASH_ENTER, &found);

	/*
	 * Rebuild an invalidated connection now, at statement start, where no live
	 * statement still references the old one (the invalidation callback only
	 * flags it; it never frees, to avoid a use-after-free of a held pointer).
	 */
	if (found && entry->conn != NULL && entry->conn->invalidated)
	{
		free_conn(entry->conn);
		entry->conn = NULL;
	}

	if (!found || entry->conn == NULL)
		entry->conn = make_conn(opts);

	return entry->conn;
}

/* Apply per-request curl options (URL, TLS, timeouts, headers). */
static void
apply_common_opts(EtcdConn *conn, const char *url, struct curl_slist *headers,
				  RecvBuf *rb, const char *body)
{
	CURL	   *c = conn->curl;

	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_POST, 1L);
	curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE, (long) strlen(body));
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, recv_callback);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, rb);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, conn->connect_timeout_ms);
	curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, conn->request_timeout_ms);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);

	if (conn->use_tls)
	{
		if (conn->cafile)
			curl_easy_setopt(c, CURLOPT_CAINFO, conn->cafile);
		if (conn->certfile)
			curl_easy_setopt(c, CURLOPT_SSLCERT, conn->certfile);
		if (conn->keyfile)
			curl_easy_setopt(c, CURLOPT_SSLKEY, conn->keyfile);
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER,
						 conn->tls_verify ? 1L : 0L);
		curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST,
						 conn->tls_verify ? 2L : 0L);
	}
}

/*
 * Perform a single POST against a specific base URL.  Returns true on a
 * completed HTTP exchange (status set), false on transport failure (so the
 * caller can fail over).  On false, *errbuf describes the transport error.
 */
static bool
do_post(EtcdConn *conn, const char *base, const char *path,
		const char *body, EtcdHttpResponse *resp, char *errbuf, size_t errlen)
{
	CURLcode	rc;
	struct curl_slist *headers = NULL;
	RecvBuf		rb;
	char	   *url = psprintf("%s%s", base, path);
	long		status = 0;

	initStringInfo(&rb.buf);

	headers = curl_slist_append(headers, "Content-Type: application/json");
	if (conn->token)
	{
		char	   *auth = psprintf("Authorization: %s", conn->token);

		headers = curl_slist_append(headers, auth);
		pfree(auth);
	}

	curl_easy_reset(conn->curl);
	apply_common_opts(conn, url, headers, &rb, body);

	rc = curl_easy_perform(conn->curl);
	curl_easy_getinfo(conn->curl, CURLINFO_RESPONSE_CODE, &status);

	curl_slist_free_all(headers);
	pfree(url);

	if (rc != CURLE_OK)
	{
		snprintf(errbuf, errlen, "%s", curl_easy_strerror(rc));
		pfree(rb.buf.data);
		return false;
	}

	resp->status = status;
	resp->body = rb.buf.data;	/* already NUL-terminated by StringInfo */
	resp->body_len = rb.buf.len;
	return true;
}

/*
 * Authenticate and cache the token.  An etcd auth token is valid cluster-wide,
 * so try every endpoint until one answers: a single dead node must never make
 * an authenticated cluster unreachable.  The endpoint that worked becomes the
 * current one.
 */
static bool
authenticate(EtcdConn *conn)
{
	cJSON	   *req;
	char	   *body;
	int			n = list_length(conn->endpoints);
	int			tried;
	bool		got = false;

	if (conn->username == NULL)
		return false;			/* nothing to do */

	req = cJSON_CreateObject();
	cJSON_AddStringToObject(req, "name", conn->username);
	cJSON_AddStringToObject(req, "password",
							conn->password ? conn->password : "");
	body = cJSON_PrintUnformatted(req);
	cJSON_Delete(req);

	for (tried = 0; tried < n && !got; tried++)
	{
		int			idx = (conn->cur_endpoint + tried) % n;
		const char *base = (char *) list_nth(conn->endpoints, idx);
		EtcdHttpResponse resp;
		char		errbuf[CURL_ERROR_SIZE];

		if (!do_post(conn, base, "/v3/auth/authenticate", body, &resp,
					 errbuf, sizeof(errbuf)))
			continue;			/* node down: try the next endpoint */

		if (resp.status == 200)
		{
			cJSON	   *root = cJSON_Parse(resp.body);
			const char *tok = root ? etcd_json_get_string(root, "token") : NULL;

			if (tok)
			{
				MemoryContext old = MemoryContextSwitchTo(EtcdCacheContext);

				conn->token = pstrdup(tok);
				MemoryContextSwitchTo(old);
				conn->cur_endpoint = idx;	/* remember the working node */
				got = true;
			}
			if (root)
				cJSON_Delete(root);
		}
		if (resp.body)
			pfree(resp.body);
	}

	pfree(body);					/* cJSON_PrintUnformatted uses malloc */
	return got;
}

/* copy the value part of a "Key:value" certinfo line if the key matches */
static void
match_certinfo(const char *line, const char *key, char **dst)
{
	size_t		klen = strlen(key);

	if (pg_strncasecmp(line, key, klen) == 0 && line[klen] == ':')
		*dst = pstrdup(line + klen + 1);
}

EtcdCertChain *
etcd_conn_probe_certinfo(EtcdConn *conn)
{
	EtcdCertChain *chain = palloc0(sizeof(EtcdCertChain));
	const char *base = (char *) list_nth(conn->endpoints, conn->cur_endpoint);
	char	   *url = psprintf("%s/v3/maintenance/status", base);
	struct curl_slist *headers = NULL;
	struct curl_certinfo *ci = NULL;
	RecvBuf		rb;
	CURLcode	rc;
	CURL	   *c;

	/*
	 * Use a dedicated, short-lived handle: CURLINFO_CERTINFO is only populated
	 * by a fresh TLS handshake, so reusing the cached keep-alive data handle is
	 * unreliable.  A separate handle also avoids disturbing data requests.
	 */
	c = curl_easy_init();
	if (c == NULL)
	{
		chain->ok = false;
		snprintf(chain->errmsg, sizeof(chain->errmsg), "could not create curl handle");
		pfree(url);
		return chain;
	}

	initStringInfo(&rb.buf);
	headers = curl_slist_append(headers, "Content-Type: application/json");

	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_POST, 1L);
	curl_easy_setopt(c, CURLOPT_POSTFIELDS, "{}");
	curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, recv_callback);
	curl_easy_setopt(c, CURLOPT_WRITEDATA, &rb);
	curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, conn->connect_timeout_ms);
	curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, conn->request_timeout_ms);
	curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(c, CURLOPT_CERTINFO, 1L);
	/* inspect the presented cert regardless of trust, so expiry is visible */
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
	curl_easy_setopt(c, CURLOPT_SSL_VERIFYHOST, 0L);
	if (conn->cafile)
		curl_easy_setopt(c, CURLOPT_CAINFO, conn->cafile);
	if (conn->certfile)
		curl_easy_setopt(c, CURLOPT_SSLCERT, conn->certfile);
	if (conn->keyfile)
		curl_easy_setopt(c, CURLOPT_SSLKEY, conn->keyfile);

	rc = curl_easy_perform(c);

	if (rc == CURLE_OK &&
		curl_easy_getinfo(c, CURLINFO_CERTINFO, &ci) == CURLE_OK &&
		ci != NULL && ci->num_of_certs > 0)
	{
		int			i;

		chain->n = ci->num_of_certs;
		chain->certs = palloc0(sizeof(EtcdCertEntry) * chain->n);
		for (i = 0; i < ci->num_of_certs; i++)
		{
			struct curl_slist *node;

			for (node = ci->certinfo[i]; node != NULL; node = node->next)
			{
				match_certinfo(node->data, "Subject", &chain->certs[i].subject);
				match_certinfo(node->data, "Issuer", &chain->certs[i].issuer);
				match_certinfo(node->data, "Start date", &chain->certs[i].start_date);
				match_certinfo(node->data, "Expire date", &chain->certs[i].expire_date);
			}
		}
		chain->ok = true;
	}
	else
	{
		chain->ok = false;
		snprintf(chain->errmsg, sizeof(chain->errmsg), "%s",
				 rc != CURLE_OK ? curl_easy_strerror(rc) : "no certificate info");
	}

	curl_slist_free_all(headers);
	curl_easy_cleanup(c);
	pfree(url);
	pfree(rb.buf.data);
	return chain;
}

/* HTTP statuses worth retrying on another node / after a backoff. */
static inline bool
status_is_retryable(long status)
{
	return status >= 500 || status == 429;
}

EtcdHttpResponse
etcd_conn_post(EtcdConn *conn, const char *path, const char *json_body)
{
	EtcdHttpResponse resp = {0};
	int			n = list_length(conn->endpoints);
	int			attempt;
	int			max_attempts = (int) conn->max_retries + 1;
	char		last_err[CURL_ERROR_SIZE] = "no endpoint reachable";

	/* If credentials are configured but we have no token yet, get one. */
	if (conn->username && conn->token == NULL)
		(void) authenticate(conn);

	/*
	 * Resilience: walk the whole endpoint list, failing over not just on
	 * transport errors but also on HTTP 5xx/429 (etcd briefly unavailable, e.g.
	 * a leader election).  If a full pass turns up nothing usable, back off and
	 * retry the list up to max_retries times so a short outage is ridden out.
	 */
	for (attempt = 0; attempt < max_attempts; attempt++)
	{
		int			tried;

		for (tried = 0; tried < n; tried++)
		{
			int			idx = (conn->cur_endpoint + tried) % n;
			const char *base = (char *) list_nth(conn->endpoints, idx);

			if (!do_post(conn, base, path, json_body, &resp,
						 last_err, sizeof(last_err)))
				continue;		/* transport failure: try the next endpoint */

			conn->cur_endpoint = idx;

			/* Transparent single re-auth on 401. */
			if (resp.status == 401 && conn->username)
			{
				pfree(resp.body);
				resp.body = NULL;
				conn->token = NULL;
				if (!authenticate(conn) ||
					!do_post(conn, base, path, json_body, &resp,
							 last_err, sizeof(last_err)))
					continue;	/* re-auth failed; try next endpoint */
			}

			if (status_is_retryable(resp.status))
			{
				snprintf(last_err, sizeof(last_err), "HTTP %ld from %s",
						 resp.status, base);
				pfree(resp.body);
				resp.body = NULL;
				continue;		/* server-side unavailable: fail over */
			}

			return resp;		/* 2xx or a definitive 4xx */
		}

		/* whole list exhausted; back off before the next pass */
		if (attempt + 1 < max_attempts && conn->retry_backoff_ms > 0)
		{
			CHECK_FOR_INTERRUPTS();
			pg_usleep((long) (attempt + 1) * conn->retry_backoff_ms * 1000L);
		}
		CHECK_FOR_INTERRUPTS();
	}

	ereport(ERROR,
			(errcode(ERRCODE_FDW_UNABLE_TO_ESTABLISH_CONNECTION),
			 errmsg("etcd_fdw: no etcd endpoint available after %d attempt(s)",
					max_attempts),
			 errdetail("last error: %s", last_err)));
	return resp;				/* unreachable */
}

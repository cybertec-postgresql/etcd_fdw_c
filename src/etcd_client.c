/*-------------------------------------------------------------------------
 *
 * etcd_client.c
 *	  High-level etcd v3 operations over the JSON gateway.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "lib/stringinfo.h"
#include "utils/builtins.h"

#include "cJSON.h"
#include "etcd_client.h"
#include "etcd_json.h"

/* Map our sort enums to the gateway's string names. */
static const char *
sort_order_name(EtcdSortOrder o)
{
	switch (o)
	{
		case ETCD_SORT_ASCEND:
			return "ASCEND";
		case ETCD_SORT_DESCEND:
			return "DESCEND";
		default:
			return "NONE";
	}
}

static const char *
sort_target_name(EtcdSortTarget t)
{
	switch (t)
	{
		case ETCD_TARGET_VERSION:
			return "VERSION";
		case ETCD_TARGET_CREATE:
			return "CREATE";
		case ETCD_TARGET_MOD:
			return "MOD";
		case ETCD_TARGET_VALUE:
			return "VALUE";
		default:
			return "KEY";
	}
}

/*
 * Raise an error if the HTTP response is not a success, decoding etcd's JSON
 * error payload when present.  Returns the parsed root on success (caller must
 * cJSON_Delete it).
 */
static cJSON *
parse_ok(EtcdHttpResponse *resp, const char *op)
{
	cJSON	   *root;

	root = resp->body ? cJSON_Parse(resp->body) : NULL;

	/*
	 * cJSON_Parse copies everything it needs into the (palloc'd) tree, so the
	 * raw HTTP body buffer is dead weight from here on.  Free it now rather
	 * than letting one buffer per request pile up in the surrounding context
	 * for the life of the statement (notably under pagination and
	 * parameterized nested-loop joins, which issue many requests per query).
	 */
	if (resp->body)
	{
		pfree(resp->body);
		resp->body = NULL;
	}

	if (resp->status != 200)
	{
		char	   *msg = NULL;

		/*
		 * Copy the message into memory we own, then drop the parse tree before
		 * raising so it does not linger in the surrounding context until the
		 * statement ends.
		 */
		if (root)
		{
			const char *m = etcd_json_get_string(root, "message");

			if (m == NULL)
				m = etcd_json_get_string(root, "error");
			if (m)
				msg = pstrdup(m);
			cJSON_Delete(root);
		}
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("etcd_fdw: %s failed (HTTP %ld)", op, resp->status),
				 msg ? errdetail("%s", msg) : 0));
	}

	if (root == NULL)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("etcd_fdw: %s returned an unparseable response", op)));

	return root;
}

char *
etcd_client_version(EtcdConn *conn)
{
	EtcdHttpResponse resp;
	cJSON	   *root;
	const char *ver;
	char	   *result;

	resp = etcd_conn_post(conn, "/v3/maintenance/status", "{}");
	root = parse_ok(&resp, "status");

	ver = etcd_json_get_string(root, "version");
	result = pstrdup(ver ? ver : "unknown");

	cJSON_Delete(root);
	return result;
}

EtcdRangeResult *
etcd_client_range(EtcdConn *conn, const EtcdRangeRequest *req)
{
	EtcdRangeResult *result = palloc0(sizeof(EtcdRangeResult));
	EtcdHttpResponse resp;
	cJSON	   *root;
	cJSON	   *header;
	cJSON	   *kvs;
	cJSON	   *more;
	cJSON	   *cnt;
	char	   *body;
	cJSON	   *jreq = cJSON_CreateObject();

	cJSON_AddStringToObject(jreq, "key", etcd_b64_encode(req->key, req->key_len));
	if (req->range_end != NULL)
		cJSON_AddStringToObject(jreq, "range_end",
								etcd_b64_encode(req->range_end, req->range_end_len));
	if (req->limit > 0)
		cJSON_AddStringToObject(jreq, "limit", psprintf(INT64_FORMAT, req->limit));
	if (req->keys_only)
		cJSON_AddBoolToObject(jreq, "keys_only", true);
	if (req->count_only)
		cJSON_AddBoolToObject(jreq, "count_only", true);
	if (req->sort_order != ETCD_SORT_NONE)
	{
		cJSON_AddStringToObject(jreq, "sort_order", sort_order_name(req->sort_order));
		cJSON_AddStringToObject(jreq, "sort_target", sort_target_name(req->sort_target));
	}

	body = cJSON_PrintUnformatted(jreq);
	cJSON_Delete(jreq);

	resp = etcd_conn_post(conn, "/v3/kv/range", body);
	pfree(body);

	root = parse_ok(&resp, "range");

	header = cJSON_GetObjectItemCaseSensitive(root, "header");
	if (header)
		result->header_revision = etcd_json_get_int64(header, "revision", 0);

	more = cJSON_GetObjectItemCaseSensitive(root, "more");
	result->more = (more && cJSON_IsTrue(more));

	cnt = cJSON_GetObjectItemCaseSensitive(root, "count");
	if (cnt)
		result->count_total = etcd_json_get_int64(root, "count", 0);

	kvs = cJSON_GetObjectItemCaseSensitive(root, "kvs");
	if (kvs && cJSON_IsArray(kvs))
	{
		int			n = cJSON_GetArraySize(kvs);
		int			i = 0;
		cJSON	   *item;

		result->kvs = palloc0(sizeof(EtcdKV) * Max(n, 1));
		cJSON_ArrayForEach(item, kvs)
		{
			EtcdKV	   *kv = &result->kvs[i++];

			kv->key = etcd_json_get_b64(item, "key", &kv->key_len);
			/* a well-formed etcd kv always has a key; never hand NULL downstream */
			if (kv->key == NULL)
			{
				kv->key = pstrdup("");
				kv->key_len = 0;
			}
			kv->value = etcd_json_get_b64(item, "value", &kv->value_len);
			kv->create_revision = etcd_json_get_int64(item, "create_revision", 0);
			kv->mod_revision = etcd_json_get_int64(item, "mod_revision", 0);
			kv->version = etcd_json_get_int64(item, "version", 0);
			kv->lease = etcd_json_get_int64(item, "lease", 0);
		}
		result->count = i;
	}

	cJSON_Delete(root);
	return result;
}

bool
etcd_client_txn(EtcdConn *conn, const EtcdCompare *cmps, int ncmp,
				const EtcdOp *ops, int nops)
{
	EtcdHttpResponse resp;
	cJSON	   *root;
	cJSON	   *jreq = cJSON_CreateObject();
	cJSON	   *jcmp = cJSON_AddArrayToObject(jreq, "compare");
	cJSON	   *jsucc = cJSON_AddArrayToObject(jreq, "success");
	char	   *body;
	cJSON	   *succeeded;
	bool		ok;
	int			i;

	for (i = 0; i < ncmp; i++)
	{
		cJSON	   *c = cJSON_CreateObject();

		cJSON_AddStringToObject(c, "key",
								etcd_b64_encode(cmps[i].key, cmps[i].key_len));
		cJSON_AddStringToObject(c, "target", "MOD");
		cJSON_AddStringToObject(c, "result", "EQUAL");
		cJSON_AddStringToObject(c, "mod_revision",
								psprintf(INT64_FORMAT, cmps[i].mod_revision));
		cJSON_AddItemToArray(jcmp, c);
	}

	for (i = 0; i < nops; i++)
	{
		cJSON	   *o = cJSON_CreateObject();

		if (ops[i].type == ETCD_OP_PUT)
		{
			cJSON	   *p = cJSON_CreateObject();

			cJSON_AddStringToObject(p, "key",
									etcd_b64_encode(ops[i].key, ops[i].key_len));
			cJSON_AddStringToObject(p, "value",
									etcd_b64_encode(ops[i].value, ops[i].value_len));
			if (ops[i].lease != 0)
				cJSON_AddStringToObject(p, "lease",
										psprintf(INT64_FORMAT, ops[i].lease));
			cJSON_AddItemToObject(o, "request_put", p);
		}
		else
		{
			cJSON	   *d = cJSON_CreateObject();

			cJSON_AddStringToObject(d, "key",
									etcd_b64_encode(ops[i].key, ops[i].key_len));
			cJSON_AddItemToObject(o, "request_delete_range", d);
		}
		cJSON_AddItemToArray(jsucc, o);
	}

	body = cJSON_PrintUnformatted(jreq);
	cJSON_Delete(jreq);

	resp = etcd_conn_post(conn, "/v3/kv/txn", body);
	pfree(body);

	root = parse_ok(&resp, "txn");
	succeeded = cJSON_GetObjectItemCaseSensitive(root, "succeeded");
	ok = (succeeded && cJSON_IsTrue(succeeded));
	cJSON_Delete(root);
	return ok;
}

int64
etcd_client_lease_grant(EtcdConn *conn, int64 ttl_seconds)
{
	EtcdHttpResponse resp;
	cJSON	   *root;
	cJSON	   *jreq = cJSON_CreateObject();
	char	   *body;
	int64		id;

	cJSON_AddStringToObject(jreq, "TTL", psprintf(INT64_FORMAT, ttl_seconds));
	cJSON_AddStringToObject(jreq, "ID", "0");	/* 0 => let etcd assign */
	body = cJSON_PrintUnformatted(jreq);
	cJSON_Delete(jreq);

	resp = etcd_conn_post(conn, "/v3/lease/grant", body);
	pfree(body);

	root = parse_ok(&resp, "lease grant");
	id = etcd_json_get_int64(root, "ID", 0);
	cJSON_Delete(root);
	return id;
}

bool
etcd_client_lease_revoke(EtcdConn *conn, int64 lease_id)
{
	EtcdHttpResponse resp;
	cJSON	   *root;
	cJSON	   *jreq = cJSON_CreateObject();
	char	   *body;

	cJSON_AddStringToObject(jreq, "ID", psprintf(INT64_FORMAT, lease_id));
	body = cJSON_PrintUnformatted(jreq);
	cJSON_Delete(jreq);

	resp = etcd_conn_post(conn, "/v3/lease/revoke", body);
	pfree(body);

	root = parse_ok(&resp, "lease revoke");
	cJSON_Delete(root);
	return true;
}

int64
etcd_client_lease_keepalive(EtcdConn *conn, int64 lease_id)
{
	EtcdHttpResponse resp;
	cJSON	   *root;
	cJSON	   *obj;
	cJSON	   *result;
	cJSON	   *jreq = cJSON_CreateObject();
	char	   *body;
	int64		ttl;

	cJSON_AddStringToObject(jreq, "ID", psprintf(INT64_FORMAT, lease_id));
	body = cJSON_PrintUnformatted(jreq);
	cJSON_Delete(jreq);

	resp = etcd_conn_post(conn, "/v3/lease/keepalive", body);
	pfree(body);

	root = parse_ok(&resp, "lease keepalive");
	/* keepalive is a streaming RPC; the gateway wraps it in a "result" object */
	result = cJSON_GetObjectItemCaseSensitive(root, "result");
	obj = result ? result : root;
	ttl = etcd_json_get_int64(obj, "TTL", -1);
	cJSON_Delete(root);
	return ttl;
}

int64
etcd_client_lease_ttl(EtcdConn *conn, int64 lease_id)
{
	EtcdHttpResponse resp;
	cJSON	   *root;
	cJSON	   *jreq = cJSON_CreateObject();
	char	   *body;
	int64		ttl;

	cJSON_AddStringToObject(jreq, "ID", psprintf(INT64_FORMAT, lease_id));
	body = cJSON_PrintUnformatted(jreq);
	cJSON_Delete(jreq);

	resp = etcd_conn_post(conn, "/v3/lease/timetolive", body);
	pfree(body);

	root = parse_ok(&resp, "lease timetolive");
	ttl = etcd_json_get_int64(root, "TTL", -1);
	cJSON_Delete(root);
	return ttl;
}

int64
etcd_client_put(EtcdConn *conn, const char *key, int key_len,
				const char *value, int value_len, int64 lease)
{
	EtcdHttpResponse resp;
	cJSON	   *root;
	cJSON	   *header;
	cJSON	   *jreq = cJSON_CreateObject();
	char	   *body;
	int64		revision = 0;

	cJSON_AddStringToObject(jreq, "key", etcd_b64_encode(key, key_len));
	cJSON_AddStringToObject(jreq, "value", etcd_b64_encode(value, value_len));
	if (lease != 0)
		cJSON_AddStringToObject(jreq, "lease", psprintf(INT64_FORMAT, lease));

	body = cJSON_PrintUnformatted(jreq);
	cJSON_Delete(jreq);

	resp = etcd_conn_post(conn, "/v3/kv/put", body);
	pfree(body);

	root = parse_ok(&resp, "put");
	header = cJSON_GetObjectItemCaseSensitive(root, "header");
	if (header)
		revision = etcd_json_get_int64(header, "revision", 0);
	cJSON_Delete(root);
	return revision;
}

int64
etcd_client_delete_range(EtcdConn *conn, const char *key, int key_len,
						 const char *range_end, int range_end_len)
{
	EtcdHttpResponse resp;
	cJSON	   *root;
	cJSON	   *jreq = cJSON_CreateObject();
	char	   *body;
	int64		deleted;

	cJSON_AddStringToObject(jreq, "key", etcd_b64_encode(key, key_len));
	if (range_end != NULL)
		cJSON_AddStringToObject(jreq, "range_end",
								etcd_b64_encode(range_end, range_end_len));

	body = cJSON_PrintUnformatted(jreq);
	cJSON_Delete(jreq);

	resp = etcd_conn_post(conn, "/v3/kv/deleterange", body);
	pfree(body);

	root = parse_ok(&resp, "deleterange");
	deleted = etcd_json_get_int64(root, "deleted", 0);
	cJSON_Delete(root);
	return deleted;
}

char *
etcd_prefix_range_end(const char *prefix, int prefix_len, int *out_len)
{
	char	   *end;
	int			i;

	/* Empty prefix => scan the whole keyspace: range_end = "\0". */
	if (prefix_len == 0)
	{
		end = palloc(1);
		end[0] = '\0';
		*out_len = 1;
		return end;
	}

	end = palloc(prefix_len);
	memcpy(end, prefix, prefix_len);

	for (i = prefix_len - 1; i >= 0; i--)
	{
		if ((unsigned char) end[i] < 0xFF)
		{
			end[i] = (char) ((unsigned char) end[i] + 1);
			*out_len = i + 1;
			return end;
		}
	}

	/* prefix was all 0xFF: also means "to the end of the keyspace". */
	end = palloc(1);
	end[0] = '\0';
	*out_len = 1;
	return end;
}

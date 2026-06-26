/*-------------------------------------------------------------------------
 *
 * etcd_conn.h
 *	  libcurl-based HTTP/JSON transport and per-backend connection cache.
 *
 *-------------------------------------------------------------------------
 */
#ifndef ETCD_FDW_CONN_H
#define ETCD_FDW_CONN_H

#include "postgres.h"
#include "options.h"

typedef struct EtcdConn EtcdConn;

typedef struct EtcdHttpResponse
{
	long		status;			/* HTTP status code, 0 if transport failed */
	char	   *body;			/* palloc'd NUL-terminated response body */
	int			body_len;
} EtcdHttpResponse;

/* One-time module setup / teardown of libcurl globals. */
extern void etcd_conn_init(void);
extern void etcd_conn_fini(void);

/*
 * Mark all cached connections in this backend for rebuild.  The next request
 * for each reconnects (re-reading TLS cert files and re-authenticating); the
 * old connection is freed then, not now, so a connection a running statement
 * still references is never freed underneath it.  Returns the number marked.
 */
extern int	etcd_conn_disconnect_all(void);

/*
 * Obtain a (cached) connection for the given resolved options.  The connection
 * lives in a long-lived context and is reused across queries in the same
 * backend.
 */
extern EtcdConn *etcd_conn_get(EtcdFdwOptions *opts);

/*
 * POST json_body to an etcd JSON-gateway path (e.g. "/v3/kv/range").  Handles
 * endpoint failover, auth-token injection, and one transparent re-auth on 401.
 * The response body is palloc'd in the current memory context.
 */
extern EtcdHttpResponse etcd_conn_post(EtcdConn *conn, const char *path,
									   const char *json_body);

/*
 * One certificate from a TLS peer chain.  Dates are the raw libcurl strings,
 * e.g. "Jun 16 12:00:00 2026 GMT".  All fields may be NULL.
 */
typedef struct EtcdCertEntry
{
	char	   *subject;
	char	   *issuer;
	char	   *start_date;
	char	   *expire_date;
} EtcdCertEntry;

typedef struct EtcdCertChain
{
	bool		ok;				/* probe completed and a chain was read */
	char		errmsg[256];	/* transport error when !ok */
	int			n;				/* number of certs; index 0 is the leaf */
	EtcdCertEntry *certs;
} EtcdCertChain;

/*
 * Connect over TLS and return the certificate chain the peer presents (via
 * libcurl CURLINFO_CERTINFO).  Verification is intentionally disabled for the
 * probe so an expired/untrusted cert can still be inspected.  Result is
 * palloc'd in the current memory context; never returns NULL.
 */
extern EtcdCertChain *etcd_conn_probe_certinfo(EtcdConn *conn);

#endif							/* ETCD_FDW_CONN_H */

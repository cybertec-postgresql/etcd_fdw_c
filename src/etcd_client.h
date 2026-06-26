/*-------------------------------------------------------------------------
 *
 * etcd_client.h
 *	  High-level etcd v3 operations (range/put/delete/txn/status) built on top
 *	  of the JSON gateway transport.  The FDW callbacks talk only to this API,
 *	  never to libcurl or JSON directly.
 *
 *-------------------------------------------------------------------------
 */
#ifndef ETCD_FDW_CLIENT_H
#define ETCD_FDW_CLIENT_H

#include "postgres.h"
#include "etcd_conn.h"

/* One key/value pair as returned by etcd. */
typedef struct EtcdKV
{
	char	   *key;
	int			key_len;
	char	   *value;
	int			value_len;
	int64		create_revision;
	int64		mod_revision;
	int64		version;
	int64		lease;
} EtcdKV;

/* sort_order / sort_target enum values for Range. */
typedef enum EtcdSortOrder
{
	ETCD_SORT_NONE = 0,
	ETCD_SORT_ASCEND,
	ETCD_SORT_DESCEND
} EtcdSortOrder;

typedef enum EtcdSortTarget
{
	ETCD_TARGET_KEY = 0,
	ETCD_TARGET_VERSION,
	ETCD_TARGET_CREATE,
	ETCD_TARGET_MOD,
	ETCD_TARGET_VALUE
} EtcdSortTarget;

typedef struct EtcdRangeRequest
{
	const char *key;
	int			key_len;
	const char *range_end;		/* NULL => single-key get */
	int			range_end_len;
	int64		limit;			/* 0 => no limit */
	bool		keys_only;
	bool		count_only;
	EtcdSortOrder sort_order;
	EtcdSortTarget sort_target;
} EtcdRangeRequest;

typedef struct EtcdRangeResult
{
	EtcdKV	   *kvs;			/* palloc'd array, length == count */
	int			count;
	bool		more;			/* etcd "more" flag: results were truncated */
	int64		header_revision;
	int64		count_total;	/* total matching keys (count_only requests) */
} EtcdRangeResult;

/* Returns the etcd server version string (palloc'd). */
extern char *etcd_client_version(EtcdConn *conn);

/* Range read.  Never returns NULL; raises on error. */
extern EtcdRangeResult *etcd_client_range(EtcdConn *conn,
										  const EtcdRangeRequest *req);

/* ---- transactions (compare-and-set) ---- */

typedef struct EtcdCompare
{
	const char *key;
	int			key_len;
	int64		mod_revision;	/* expected mod_revision (target = MOD, EQUAL) */
} EtcdCompare;

typedef enum EtcdOpType
{
	ETCD_OP_PUT,
	ETCD_OP_DELETE
} EtcdOpType;

typedef struct EtcdOp
{
	EtcdOpType	type;
	const char *key;
	int			key_len;
	const char *value;			/* PUT only */
	int			value_len;
	int64		lease;			/* PUT only, 0 == none */
} EtcdOp;

/*
 * Run a txn: if every compare holds, execute the success ops.  Returns true if
 * the comparisons succeeded (ops ran), false otherwise.  ncmp may be 0 (ops
 * always run).
 */
extern bool etcd_client_txn(EtcdConn *conn,
							const EtcdCompare *cmps, int ncmp,
							const EtcdOp *ops, int nops);

/* ---- lease management (the /v3/lease endpoints) ---- */

/* Grant a lease for ttl_seconds; returns the new lease id. */
extern int64 etcd_client_lease_grant(EtcdConn *conn, int64 ttl_seconds);

/* Revoke a lease (and delete all keys attached to it).  Returns true on success. */
extern bool etcd_client_lease_revoke(EtcdConn *conn, int64 lease_id);

/* Refresh a lease once; returns the new remaining TTL in seconds (<=0 if gone). */
extern int64 etcd_client_lease_keepalive(EtcdConn *conn, int64 lease_id);

/* Remaining TTL in seconds for a lease; -1 if it does not exist / has expired. */
extern int64 etcd_client_lease_ttl(EtcdConn *conn, int64 lease_id);

/* Put a single key.  Returns the cluster revision after the write. */
extern int64 etcd_client_put(EtcdConn *conn,
							 const char *key, int key_len,
							 const char *value, int value_len,
							 int64 lease);

/*
 * Delete a range [key, range_end).  Pass range_end == NULL to delete one key.
 * Returns the number of keys deleted.
 */
extern int64 etcd_client_delete_range(EtcdConn *conn,
									  const char *key, int key_len,
									  const char *range_end, int range_end_len);

/*
 * Compute the etcd range_end for a prefix scan: the prefix with its last byte
 * incremented (matches etcdctl "--prefix" semantics).  Returns a palloc'd
 * buffer of *out_len bytes; for an empty/all-0xFF prefix returns the special
 * "\0" range_end meaning "to the end of the keyspace".
 */
extern char *etcd_prefix_range_end(const char *prefix, int prefix_len,
								   int *out_len);

#endif							/* ETCD_FDW_CLIENT_H */

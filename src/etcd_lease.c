/*-------------------------------------------------------------------------
 *
 * etcd_lease.c
 *	  SQL-callable etcd lease management: grant, revoke, keepalive, ttl.
 *
 * These operate on a server you are entitled to use, so they require either
 * superuser or USAGE on the foreign server (the same privilege that lets you
 * read/write its foreign tables).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_foreign_server.h"
#include "fmgr.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "utils/acl.h"
#include "utils/builtins.h"

#include "compat.h"
#include "etcd_client.h"
#include "etcd_conn.h"
#include "options.h"

PG_FUNCTION_INFO_V1(etcd_fdw_lease_grant);
PG_FUNCTION_INFO_V1(etcd_fdw_lease_revoke);
PG_FUNCTION_INFO_V1(etcd_fdw_lease_keepalive);
PG_FUNCTION_INFO_V1(etcd_fdw_lease_ttl);

/* Resolve an etcd_fdw server by name, check USAGE, and return a connection. */
static EtcdConn *
lease_conn(text *srv_text)
{
	char	   *name = text_to_cstring(srv_text);
	ForeignServer *server = GetForeignServerByName(name, false);
	ForeignDataWrapper *fdw = GetForeignDataWrapper(server->fdwid);

	if (strcmp(fdw->fdwname, "etcd_fdw") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("server \"%s\" is not an etcd_fdw server", name)));

	if (!superuser() &&
		ETCD_SERVER_ACLCHECK(server->serverid, GetUserId()) != ACLCHECK_OK)
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("permission denied for etcd server \"%s\"", name),
				 errhint("Must be a superuser or have USAGE on the server.")));

	return etcd_conn_get(etcd_get_server_options(server->serverid,
												 GetUserId(), NULL));
}

Datum
etcd_fdw_lease_grant(PG_FUNCTION_ARGS)
{
	EtcdConn   *conn = lease_conn(PG_GETARG_TEXT_PP(0));
	int64		ttl = PG_GETARG_INT64(1);

	if (ttl <= 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("lease TTL must be a positive number of seconds")));

	PG_RETURN_INT64(etcd_client_lease_grant(conn, ttl));
}

Datum
etcd_fdw_lease_revoke(PG_FUNCTION_ARGS)
{
	EtcdConn   *conn = lease_conn(PG_GETARG_TEXT_PP(0));

	PG_RETURN_BOOL(etcd_client_lease_revoke(conn, PG_GETARG_INT64(1)));
}

Datum
etcd_fdw_lease_keepalive(PG_FUNCTION_ARGS)
{
	EtcdConn   *conn = lease_conn(PG_GETARG_TEXT_PP(0));

	PG_RETURN_INT64(etcd_client_lease_keepalive(conn, PG_GETARG_INT64(1)));
}

Datum
etcd_fdw_lease_ttl(PG_FUNCTION_ARGS)
{
	EtcdConn   *conn = lease_conn(PG_GETARG_TEXT_PP(0));

	PG_RETURN_INT64(etcd_client_lease_ttl(conn, PG_GETARG_INT64(1)));
}

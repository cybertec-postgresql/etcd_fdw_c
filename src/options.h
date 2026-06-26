/*-------------------------------------------------------------------------
 *
 * options.h
 *	  Option definitions, parsing and validation for etcd_fdw.
 *
 *-------------------------------------------------------------------------
 */
#ifndef ETCD_FDW_OPTIONS_H
#define ETCD_FDW_OPTIONS_H

#include "postgres.h"
#include "nodes/pg_list.h"

/* Resolved options for one foreign table, merged from server/user/table. */
typedef struct EtcdFdwOptions
{
	/* connection (server + user mapping) */
	List	   *endpoints;		/* list of char* base URLs, e.g. http://h:2379 */
	bool		use_tls;
	char	   *cafile;
	char	   *certfile;
	char	   *keyfile;
	bool		tls_verify;
	long		connect_timeout_ms;
	long		request_timeout_ms;
	long		max_retries;		/* extra full passes over the endpoint list */
	long		retry_backoff_ms;	/* base backoff between retry passes */
	char	   *username;		/* etcd RBAC, may be NULL */
	char	   *password;		/* may be NULL */

	/* table mapping */
	char	   *prefix;			/* effective key prefix (server.prefix + table.prefix) */
	bool		strip_prefix;	/* present key column with prefix removed */
	bool		use_remote_estimate;	/* count_only Range for row estimates */

	/* identity for connection cache keying */
	Oid			serverid;
	Oid			userid;
} EtcdFdwOptions;

/* Fetch and merge all options for a foreign table. */
extern EtcdFdwOptions *etcd_get_options(Oid foreigntableid, Oid userid);

/* Fetch connection options for a server (used by IMPORT FOREIGN SCHEMA). */
extern EtcdFdwOptions *etcd_get_server_options(Oid serverid, Oid userid,
											   const char *prefix);

/* Validator entry point used by etcd_fdw_validator(). */
extern void etcd_validate_options(List *options_list, Oid catalog);

#endif							/* ETCD_FDW_OPTIONS_H */

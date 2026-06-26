/*-------------------------------------------------------------------------
 *
 * options.c
 *	  Option handling for etcd_fdw.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/reloptions.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_foreign_table.h"
#include "catalog/pg_user_mapping.h"
#include "commands/defrem.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/syscache.h"
#include "utils/varlena.h"

#include "options.h"

/* Defaults */
#define ETCD_DEFAULT_ENDPOINT		"http://127.0.0.1:2379"
#define ETCD_DEFAULT_CONNECT_MS		3000
#define ETCD_DEFAULT_REQUEST_MS		30000
#define ETCD_DEFAULT_MAX_RETRIES	2
#define ETCD_DEFAULT_RETRY_BACKOFF_MS 200

static bool
is_valid_option(const char *name, Oid context)
{
	/* Server-level options */
	if (context == ForeignServerRelationId)
	{
		return strcmp(name, "endpoints") == 0 ||
			strcmp(name, "use_tls") == 0 ||
			strcmp(name, "cafile") == 0 ||
			strcmp(name, "certfile") == 0 ||
			strcmp(name, "keyfile") == 0 ||
			strcmp(name, "tls_verify") == 0 ||
			strcmp(name, "connect_timeout_ms") == 0 ||
			strcmp(name, "request_timeout_ms") == 0 ||
			strcmp(name, "max_retries") == 0 ||
			strcmp(name, "retry_backoff_ms") == 0 ||
			strcmp(name, "use_remote_estimate") == 0 ||
			strcmp(name, "prefix") == 0;
	}
	/* Foreign-data-wrapper level: accept the same connection options */
	if (context == ForeignDataWrapperRelationId)
	{
		return strcmp(name, "endpoints") == 0 ||
			strcmp(name, "use_tls") == 0 ||
			strcmp(name, "tls_verify") == 0 ||
			strcmp(name, "connect_timeout_ms") == 0 ||
			strcmp(name, "request_timeout_ms") == 0 ||
			strcmp(name, "max_retries") == 0 ||
			strcmp(name, "retry_backoff_ms") == 0;
	}
	/* User mapping options */
	if (context == UserMappingRelationId)
	{
		return strcmp(name, "username") == 0 ||
			strcmp(name, "password") == 0;
	}
	/* Foreign table options */
	if (context == ForeignTableRelationId)
	{
		return strcmp(name, "prefix") == 0 ||
			strcmp(name, "strip_prefix") == 0 ||
			strcmp(name, "use_remote_estimate") == 0;
	}
	/* Column options (AttributeRelationId): no custom options yet */
	return false;
}

void
etcd_validate_options(List *options_list, Oid catalog)
{
	ListCell   *cell;

	foreach(cell, options_list)
	{
		DefElem    *def = (DefElem *) lfirst(cell);

		if (!is_valid_option(def->defname, catalog))
			ereport(ERROR,
					(errcode(ERRCODE_FDW_INVALID_OPTION_NAME),
					 errmsg("invalid option \"%s\"", def->defname),
					 errhint("There are no valid options in this context, or the option is unknown.")));

		/* Type-check a few options eagerly so DDL fails fast. */
		if (strcmp(def->defname, "use_tls") == 0 ||
			strcmp(def->defname, "tls_verify") == 0 ||
			strcmp(def->defname, "strip_prefix") == 0 ||
			strcmp(def->defname, "use_remote_estimate") == 0)
		{
			(void) defGetBoolean(def);
		}
		else if (strcmp(def->defname, "connect_timeout_ms") == 0 ||
				 strcmp(def->defname, "request_timeout_ms") == 0)
		{
			char	   *val = defGetString(def);
			long		n = strtol(val, NULL, 10);

			if (n <= 0)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
						 errmsg("option \"%s\" must be a positive integer (milliseconds)",
								def->defname)));
		}
		else if (strcmp(def->defname, "max_retries") == 0 ||
				 strcmp(def->defname, "retry_backoff_ms") == 0)
		{
			char	   *val = defGetString(def);
			long		n = strtol(val, NULL, 10);

			if (n < 0)
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
						 errmsg("option \"%s\" must not be negative",
								def->defname)));
		}
		else if (strcmp(def->defname, "endpoints") == 0)
		{
			char	   *val = defGetString(def);

			if (val == NULL || val[0] == '\0')
				ereport(ERROR,
						(errcode(ERRCODE_FDW_INVALID_ATTRIBUTE_VALUE),
						 errmsg("option \"endpoints\" must not be empty")));
		}
	}
}

/* Split "a,b , c" into a List of palloc'd trimmed strings. */
static List *
split_endpoints(const char *raw)
{
	List	   *result = NIL;
	char	   *copy = pstrdup(raw);
	char	   *tok;
	char	   *saveptr = NULL;

	for (tok = strtok_r(copy, ",", &saveptr); tok != NULL;
		 tok = strtok_r(NULL, ",", &saveptr))
	{
		/* trim leading/trailing spaces */
		while (*tok == ' ' || *tok == '\t')
			tok++;
		{
			int			len = (int) strlen(tok);

			while (len > 0 && (tok[len - 1] == ' ' || tok[len - 1] == '\t'))
				tok[--len] = '\0';
		}
		if (*tok == '\0')
			continue;

		/* If it has no scheme, assume http:// */
		if (strstr(tok, "://") == NULL)
			result = lappend(result, psprintf("http://%s", tok));
		else
			result = lappend(result, pstrdup(tok));
	}
	return result;
}

/*
 * Resolve the connection options (FDW + server + user mapping) into opts and
 * return the server-level prefix option (or NULL).  Shared by the table and
 * server-only entry points.
 */
static char *
fill_conn_options(EtcdFdwOptions *opts, ForeignServer *server, Oid userid)
{
	ForeignDataWrapper *wrapper = GetForeignDataWrapper(server->fdwid);
	UserMapping *mapping = NULL;
	List	   *all;
	ListCell   *lc;
	const char *raw_endpoints = NULL;
	char	   *server_prefix = NULL;

	/* defaults */
	opts->use_tls = false;
	opts->tls_verify = true;
	opts->connect_timeout_ms = ETCD_DEFAULT_CONNECT_MS;
	opts->request_timeout_ms = ETCD_DEFAULT_REQUEST_MS;
	opts->max_retries = ETCD_DEFAULT_MAX_RETRIES;
	opts->retry_backoff_ms = ETCD_DEFAULT_RETRY_BACKOFF_MS;
	opts->strip_prefix = false;
	opts->use_remote_estimate = false;
	opts->serverid = server->serverid;
	opts->userid = userid;

	/*
	 * The user mapping is optional (etcd may need no auth).  Probe the syscache
	 * for a mapping for this user, then for a PUBLIC mapping, without throwing:
	 * GetUserMapping() raises an error when none exists, and catching that
	 * error mid-planning is not safe.
	 */
	if (OidIsValid(GetSysCacheOid2(USERMAPPINGUSERSERVER,
								   Anum_pg_user_mapping_oid,
								   ObjectIdGetDatum(userid),
								   ObjectIdGetDatum(server->serverid))) ||
		OidIsValid(GetSysCacheOid2(USERMAPPINGUSERSERVER,
								   Anum_pg_user_mapping_oid,
								   ObjectIdGetDatum(InvalidOid),
								   ObjectIdGetDatum(server->serverid))))
		mapping = GetUserMapping(userid, server->serverid);

	/* Apply FDW-level, then server-level, then user-mapping options. */
	all = NIL;
	all = list_concat(all, list_copy(wrapper->options));
	all = list_concat(all, list_copy(server->options));
	if (mapping)
		all = list_concat(all, list_copy(mapping->options));

	foreach(lc, all)
	{
		DefElem    *def = (DefElem *) lfirst(lc);
		const char *name = def->defname;

		if (strcmp(name, "endpoints") == 0)
			raw_endpoints = defGetString(def);
		else if (strcmp(name, "use_tls") == 0)
			opts->use_tls = defGetBoolean(def);
		else if (strcmp(name, "cafile") == 0)
			opts->cafile = defGetString(def);
		else if (strcmp(name, "certfile") == 0)
			opts->certfile = defGetString(def);
		else if (strcmp(name, "keyfile") == 0)
			opts->keyfile = defGetString(def);
		else if (strcmp(name, "tls_verify") == 0)
			opts->tls_verify = defGetBoolean(def);
		else if (strcmp(name, "connect_timeout_ms") == 0)
			opts->connect_timeout_ms = strtol(defGetString(def), NULL, 10);
		else if (strcmp(name, "request_timeout_ms") == 0)
			opts->request_timeout_ms = strtol(defGetString(def), NULL, 10);
		else if (strcmp(name, "max_retries") == 0)
			opts->max_retries = strtol(defGetString(def), NULL, 10);
		else if (strcmp(name, "retry_backoff_ms") == 0)
			opts->retry_backoff_ms = strtol(defGetString(def), NULL, 10);
		else if (strcmp(name, "use_remote_estimate") == 0)
			opts->use_remote_estimate = defGetBoolean(def);
		else if (strcmp(name, "prefix") == 0)
			server_prefix = defGetString(def);
		else if (strcmp(name, "username") == 0)
			opts->username = defGetString(def);
		else if (strcmp(name, "password") == 0)
			opts->password = defGetString(def);
	}

	/* endpoints */
	opts->endpoints = split_endpoints(raw_endpoints ? raw_endpoints
									  : ETCD_DEFAULT_ENDPOINT);
	if (opts->endpoints == NIL)
		opts->endpoints = list_make1(pstrdup(ETCD_DEFAULT_ENDPOINT));

	return server_prefix;
}

EtcdFdwOptions *
etcd_get_options(Oid foreigntableid, Oid userid)
{
	EtcdFdwOptions *opts = palloc0(sizeof(EtcdFdwOptions));
	ForeignTable *table = GetForeignTable(foreigntableid);
	ForeignServer *server = GetForeignServer(table->serverid);
	ListCell   *lc;
	char	   *server_prefix;
	char	   *table_prefix = NULL;

	server_prefix = fill_conn_options(opts, server, userid);

	/* table-level options */
	foreach(lc, table->options)
	{
		DefElem    *def = (DefElem *) lfirst(lc);

		if (strcmp(def->defname, "prefix") == 0)
			table_prefix = defGetString(def);
		else if (strcmp(def->defname, "strip_prefix") == 0)
			opts->strip_prefix = defGetBoolean(def);
		else if (strcmp(def->defname, "use_remote_estimate") == 0)
			opts->use_remote_estimate = defGetBoolean(def);
	}

	/* effective prefix = server prefix concatenated with table prefix */
	if (server_prefix && table_prefix)
		opts->prefix = psprintf("%s%s", server_prefix, table_prefix);
	else if (server_prefix)
		opts->prefix = pstrdup(server_prefix);
	else if (table_prefix)
		opts->prefix = pstrdup(table_prefix);
	else
		opts->prefix = pstrdup("");

	return opts;
}

EtcdFdwOptions *
etcd_get_server_options(Oid serverid, Oid userid, const char *prefix)
{
	EtcdFdwOptions *opts = palloc0(sizeof(EtcdFdwOptions));
	ForeignServer *server = GetForeignServer(serverid);
	char	   *server_prefix;

	server_prefix = fill_conn_options(opts, server, userid);

	if (server_prefix && prefix)
		opts->prefix = psprintf("%s%s", server_prefix, prefix);
	else if (prefix)
		opts->prefix = pstrdup(prefix);
	else if (server_prefix)
		opts->prefix = pstrdup(server_prefix);
	else
		opts->prefix = pstrdup("");

	return opts;
}

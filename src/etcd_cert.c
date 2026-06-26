/*-------------------------------------------------------------------------
 *
 * etcd_cert.c
 *	  SQL-callable functions reporting TLS certificate validity/expiry for an
 *	  etcd server: the peer certificate it presents (via libcurl CERTINFO) and
 *	  the locally configured CA bundle / client cert (via OpenSSL).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <time.h>

#include <openssl/pem.h>
#include <openssl/x509.h>

#include "catalog/pg_authid.h"
#include "catalog/pg_foreign_server.h"
#include "funcapi.h"
#include "foreign/foreign.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "storage/fd.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/timestamp.h"

#include "compat.h"
#include "etcd_conn.h"
#include "options.h"

PG_FUNCTION_INFO_V1(etcd_fdw_certificates);
PG_FUNCTION_INFO_V1(etcd_fdw_server_cert_expiry);
PG_FUNCTION_INFO_V1(etcd_fdw_disconnect);

/*
 * Mark this backend's cached etcd connections for rebuild; the next query
 * reconnects, re-reading TLS certificate files and re-authenticating.  Useful
 * after an in-place certificate rotation.  Returns the number marked.
 */
Datum
etcd_fdw_disconnect(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(etcd_conn_disconnect_all());
}

/* Caller must be superuser, a pg_monitor member, or have USAGE on the server. */
static void
require_cert_access(Oid serverid)
{
	Oid			uid = GetUserId();

	if (superuser())
		return;
	if (has_privs_of_role(uid, ROLE_PG_MONITOR))
		return;
	if (ETCD_SERVER_ACLCHECK(serverid, uid) == ACLCHECK_OK)
		return;

	ereport(ERROR,
			(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
			 errmsg("permission denied to inspect certificates for etcd server"),
			 errhint("Must be a superuser, a member of pg_monitor, or have USAGE on the server.")));
}

/* Build a UTC TimestampTz from a broken-down UTC time. */
static TimestampTz
tm_utc_to_tstz(const struct tm *tm)
{
	char		buf[64];

	snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d+00",
			 tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
			 tm->tm_hour, tm->tm_min, tm->tm_sec);
	return DatumGetTimestampTz(DirectFunctionCall3(timestamptz_in,
												   CStringGetDatum(buf),
												   ObjectIdGetDatum(InvalidOid),
												   Int32GetDatum(-1)));
}

/* Parse libcurl's "Jun 16 12:00:00 2026 GMT" form (always GMT/UTC). */
static bool
parse_curl_date(const char *s, TimestampTz *out)
{
	struct tm	tm;

	if (s == NULL)
		return false;
	memset(&tm, 0, sizeof(tm));
	if (strptime(s, "%b %d %H:%M:%S %Y", &tm) == NULL)
		return false;
	*out = tm_utc_to_tstz(&tm);
	return true;
}

/* ASN1_TIME (UTC) -> TimestampTz */
static bool
asn1_to_tstz(const ASN1_TIME *t, TimestampTz *out)
{
	struct tm	tm;

	if (t == NULL)
		return false;
	memset(&tm, 0, sizeof(tm));
	if (ASN1_TIME_to_tm(t, &tm) != 1)
		return false;
	*out = tm_utc_to_tstz(&tm);
	return true;
}

/* expires_in = not_after - now() as an Interval datum */
static Datum
expires_in_datum(TimestampTz not_after)
{
	return DirectFunctionCall2(timestamp_mi,
							   TimestampTzGetDatum(not_after),
							   TimestampTzGetDatum(GetCurrentTimestamp()));
}

/* emit one (source, subject, issuer, not_before, not_after, expires_in) row */
static void
put_cert_row(Tuplestorestate *tupstore, TupleDesc tupdesc,
			 const char *source, const char *subject, const char *issuer,
			 bool nb_ok, TimestampTz nb, bool na_ok, TimestampTz na)
{
	Datum		values[6];
	bool		nulls[6];

	memset(nulls, false, sizeof(nulls));
	values[0] = CStringGetTextDatum(source);
	if (subject)
		values[1] = CStringGetTextDatum(subject);
	else
		nulls[1] = true;
	if (issuer)
		values[2] = CStringGetTextDatum(issuer);
	else
		nulls[2] = true;
	if (nb_ok)
		values[3] = TimestampTzGetDatum(nb);
	else
		nulls[3] = true;
	if (na_ok)
		values[4] = TimestampTzGetDatum(na);
	else
		nulls[4] = true;
	if (na_ok)
		values[5] = expires_in_datum(na);
	else
		nulls[5] = true;

	tuplestore_putvalues(tupstore, tupdesc, values, nulls);
}

/* Parse a local PEM file (possibly a bundle) and emit a row per certificate. */
static void
add_file_certs(Tuplestorestate *tupstore, TupleDesc tupdesc,
			   const char *path, const char *source)
{
	FILE	   *fp;
	X509	   *x;

	if (path == NULL || path[0] == '\0')
		return;

	fp = AllocateFile(path, PG_BINARY_R);
	if (fp == NULL)
	{
		ereport(WARNING,
				(errcode_for_file_access(),
				 errmsg("etcd_fdw: could not open certificate file \"%s\": %m",
						path)));
		return;
	}

	while ((x = PEM_read_X509(fp, NULL, NULL, NULL)) != NULL)
	{
		char		subj[256];
		char		iss[256];
		TimestampTz nb = 0;
		TimestampTz na = 0;
		bool		nb_ok;
		bool		na_ok;

		/*
		 * Extract everything we need into local storage, then free the X509
		 * before doing anything that can ereport.  asn1_to_tstz feeds the cert
		 * date through timestamptz_in, which raises on a malformed/out-of-range
		 * date; the X509 is OpenSSL-malloc'd (outside any palloc context), so we
		 * must free it on that path too, hence the PG_TRY.  put_cert_row runs
		 * after the free, so its own potential error cannot leak the cert.
		 */
		PG_TRY();
		{
			X509_NAME_oneline(X509_get_subject_name(x), subj, sizeof(subj));
			X509_NAME_oneline(X509_get_issuer_name(x), iss, sizeof(iss));
			nb_ok = asn1_to_tstz(X509_get0_notBefore(x), &nb);
			na_ok = asn1_to_tstz(X509_get0_notAfter(x), &na);
		}
		PG_CATCH();
		{
			X509_free(x);
			PG_RE_THROW();
		}
		PG_END_TRY();
		X509_free(x);

		put_cert_row(tupstore, tupdesc, source, subj, iss, nb_ok, nb, na_ok, na);
	}
	FreeFile(fp);
}

/* Resolve a server by name, ensure it belongs to etcd_fdw, check access. */
static ForeignServer *
get_checked_server(const char *name)
{
	ForeignServer *server = GetForeignServerByName(name, false);
	ForeignDataWrapper *fdw = GetForeignDataWrapper(server->fdwid);

	if (strcmp(fdw->fdwname, "etcd_fdw") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("server \"%s\" is not an etcd_fdw server", name)));

	require_cert_access(server->serverid);
	return server;
}

Datum
etcd_fdw_certificates(PG_FUNCTION_ARGS)
{
	char	   *srvname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ForeignServer *server = get_checked_server(srvname);
	EtcdFdwOptions *opts = etcd_get_server_options(server->serverid, GetUserId(), NULL);
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;

	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo) ||
		!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (get_call_result_type(fcinfo, NULL, &tupdesc) != TYPEFUNC_COMPOSITE)
		elog(ERROR, "return type must be a row type");

	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);
	tupstore = tuplestore_begin_heap(true, false, work_mem);
	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;
	MemoryContextSwitchTo(oldcontext);

	/* peer certificate chain (only meaningful when TLS is configured) */
	if (opts->use_tls)
	{
		EtcdConn   *conn = etcd_conn_get(opts);
		EtcdCertChain *chain = etcd_conn_probe_certinfo(conn);

		if (chain->ok)
		{
			int			i;

			for (i = 0; i < chain->n; i++)
			{
				EtcdCertEntry *e = &chain->certs[i];
				TimestampTz nb = 0;
				TimestampTz na = 0;
				bool		nb_ok = parse_curl_date(e->start_date, &nb);
				bool		na_ok = parse_curl_date(e->expire_date, &na);

				put_cert_row(tupstore, tupdesc,
							 i == 0 ? "peer" : "peer-chain",
							 e->subject, e->issuer, nb_ok, nb, na_ok, na);
			}
		}
		else
			ereport(WARNING,
					(errmsg("etcd_fdw: could not read peer certificate: %s",
							chain->errmsg)));
	}

	/* locally configured files */
	add_file_certs(tupstore, tupdesc, opts->cafile, "cafile");
	add_file_certs(tupstore, tupdesc, opts->certfile, "certfile");

	return (Datum) 0;
}

Datum
etcd_fdw_server_cert_expiry(PG_FUNCTION_ARGS)
{
	char	   *srvname = text_to_cstring(PG_GETARG_TEXT_PP(0));
	ForeignServer *server = get_checked_server(srvname);
	EtcdFdwOptions *opts = etcd_get_server_options(server->serverid, GetUserId(), NULL);
	EtcdConn   *conn;
	EtcdCertChain *chain;
	TimestampTz na;

	if (!opts->use_tls)
		PG_RETURN_NULL();

	conn = etcd_conn_get(opts);
	chain = etcd_conn_probe_certinfo(conn);
	if (!chain->ok || chain->n == 0)
		PG_RETURN_NULL();

	if (!parse_curl_date(chain->certs[0].expire_date, &na))
		PG_RETURN_NULL();

	PG_RETURN_TIMESTAMPTZ(na);
}

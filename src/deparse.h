/*-------------------------------------------------------------------------
 *
 * deparse.h
 *	  Translate WHERE clauses on the key column into an etcd Range request.
 *
 * etcd orders keys by raw byte value, so range/prefix/ORDER BY pushdown is
 * only correct when the key column uses the C or POSIX collation.  Equality
 * is pushed for ordinary deterministic collations.  Anything not pushed is
 * left for PostgreSQL to recheck.
 *
 *-------------------------------------------------------------------------
 */
#ifndef ETCD_FDW_DEPARSE_H
#define ETCD_FDW_DEPARSE_H

#include "postgres.h"

#include "nodes/pathnodes.h"
#include "nodes/pg_list.h"

#include "etcd_client.h"
#include "options.h"

/* A resolved scan: the [start, end) range (or a single key) to fetch. */
typedef struct EtcdScanSpec
{
	char	   *start;			/* range start (full etcd key) */
	int			start_len;
	char	   *end;			/* range end; NULL iff single_key */
	int			end_len;
	bool		single_key;		/* fetch exactly "start" */
	bool		empty;			/* provably no rows: skip the request */
	int64		limit;			/* 0 == no limit */
	EtcdSortOrder sort_order;	/* requested key ordering for the scan */

	/* multi-key mode: "key IN (...)" -> fetch each of these full keys */
	int			nkeys;
	char	  **keys;
	int		   *keylens;

	/*
	 * parameterized mode: the key is computed at run time from the single
	 * expression in the plan's fdw_exprs (e.g. join clause key = outer.col).
	 * The prefix is prepended at run time when strip_prefix is set.
	 */
	bool		param_key;
} EtcdScanSpec;

/* Per-relation planning state stashed in RelOptInfo->fdw_private. */
typedef struct EtcdRelInfo
{
	EtcdFdwOptions *opts;
	EtcdScanSpec *spec;
	List	   *remote_conds;	/* RestrictInfos fully handled by etcd */
	List	   *local_conds;	/* RestrictInfos needing local recheck */
	bool		pushdown_safe_order;	/* C/POSIX collation on key column */
	AttrNumber	key_attno;		/* attnum of the key column, or 0 */
	Oid			key_collation;	/* collation of the key column */
} EtcdRelInfo;

/*
 * Analyse the base restrictions and build the scan spec.  Fills relinfo with
 * the spec and the remote/local condition split.
 */
extern void etcd_deparse_analyze(PlannerInfo *root, RelOptInfo *baserel,
								 Oid foreigntableid, EtcdRelInfo *relinfo);

/* Serialise/deserialise the spec for the plan's fdw_private. */
extern List *etcd_serialize_spec(EtcdScanSpec *spec);
extern EtcdScanSpec *etcd_deserialize_spec(List *list);

/* True if the collation orders text by raw bytes (C / POSIX). */
extern bool etcd_collation_is_byteordered(Oid coll);

/* True if equality on this collation is byte-exact (deterministic). */
extern bool etcd_collation_is_det_eq(Oid coll);

#endif							/* ETCD_FDW_DEPARSE_H */

/*-------------------------------------------------------------------------
 *
 * compat.h
 *	  Single place for PostgreSQL 14-18 API compatibility shims.
 *
 * Every #if PG_VERSION_NUM guard needed by etcd_fdw lives here so the rest
 * of the code can be written against one API.
 *
 *-------------------------------------------------------------------------
 */
#ifndef ETCD_FDW_COMPAT_H
#define ETCD_FDW_COMPAT_H

#include "postgres.h"

#if PG_VERSION_NUM < 140000
#error "etcd_fdw requires PostgreSQL 14 or later"
#endif

/*
 * EXPLAIN headers were split out of commands/explain.h in PG18:
 * ExplainState moved to explain_state.h and the ExplainProperty* helpers to
 * explain_format.h.
 */
#if PG_VERSION_NUM >= 180000
#include "commands/explain_state.h"
#include "commands/explain_format.h"
#else
#include "commands/explain.h"
#endif

/*
 * create_foreignscan_path() gained parameters over time:
 *   - PG17 added fdw_restrictinfo (before fdw_private)
 *   - PG18 added disabled_nodes (after rows)
 * This wrapper presents one signature to the rest of the code; we never push
 * join/upper restrictinfo, so fdw_restrictinfo is always NIL and disabled_nodes
 * is always 0.
 */
#if PG_VERSION_NUM >= 180000
#define ETCD_CREATE_FOREIGNSCAN_PATH(root, rel, target, rows, startup, total, \
									 pathkeys, req_outer, outerpath, priv) \
	create_foreignscan_path((root), (rel), (target), (rows), 0, (startup), \
							(total), (pathkeys), (req_outer), (outerpath), \
							NIL, (priv))
#elif PG_VERSION_NUM >= 170000
#define ETCD_CREATE_FOREIGNSCAN_PATH(root, rel, target, rows, startup, total, \
									 pathkeys, req_outer, outerpath, priv) \
	create_foreignscan_path((root), (rel), (target), (rows), (startup), \
							(total), (pathkeys), (req_outer), (outerpath), \
							NIL, (priv))
#else
#define ETCD_CREATE_FOREIGNSCAN_PATH(root, rel, target, rows, startup, total, \
									 pathkeys, req_outer, outerpath, priv) \
	create_foreignscan_path((root), (rel), (target), (rows), (startup), \
							(total), (pathkeys), (req_outer), (outerpath), \
							(priv))
#endif

/*
 * PG18 replaced PathKey.pk_strategy (a btree StrategyNumber) with
 * pk_cmptype (a CompareType).  Descending order is BTGreaterStrategyNumber
 * pre-18 and COMPARE_GT from PG18 on.
 */
#if PG_VERSION_NUM >= 180000
#include "access/cmptype.h"
#define ETCD_PATHKEY_IS_DESC(pk) ((pk)->pk_cmptype == COMPARE_GT)
#else
#define ETCD_PATHKEY_IS_DESC(pk) ((pk)->pk_strategy == BTGreaterStrategyNumber)
#endif

/*
 * Type-specific *_aclcheck functions were replaced by the generic
 * object_aclcheck(classid, objid, roleid, mode) in PG16.
 */
#if PG_VERSION_NUM >= 160000
#define ETCD_SERVER_ACLCHECK(srvid, roleid) \
	object_aclcheck(ForeignServerRelationId, (srvid), (roleid), ACL_USAGE)
#else
#define ETCD_SERVER_ACLCHECK(srvid, roleid) \
	pg_foreign_server_aclcheck((srvid), (roleid), ACL_USAGE)
#endif

#endif							/* ETCD_FDW_COMPAT_H */

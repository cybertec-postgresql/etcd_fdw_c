/*-------------------------------------------------------------------------
 *
 * etcd_fdw.c
 *	  Foreign-data wrapper for etcd v3 key/value stores.
 *
 * A foreign table maps to an etcd key prefix; rows are
 * (key, value, create_revision, mod_revision, version, lease).  Predicates on
 * the key column are pushed down to etcd Range requests where it is safe to do
 * so (see deparse.c); everything else is rechecked by PostgreSQL.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>
#include <stdlib.h>

#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/stratnum.h"
#include "access/table.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "foreign/fdwapi.h"
#include "foreign/foreign.h"
#include "lib/stringinfo.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/appendinfo.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "compat.h"
#include "deparse.h"
#include "etcd_client.h"
#include "etcd_conn.h"
#include "etcd_fdw.h"
#include "etcd_json.h"
#include "options.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

PG_FUNCTION_INFO_V1(etcd_fdw_handler);
PG_FUNCTION_INFO_V1(etcd_fdw_validator);

/* Per-scan execution state. */
typedef struct EtcdScanState
{
	EtcdFdwOptions *opts;
	EtcdConn   *conn;
	EtcdScanSpec *spec;

	/* column mapping, indexed by (attnum - 1) */
	EtcdColKind *colkind;
	Oid		   *coltype;
	int			ncols;

	/* materialised result */
	EtcdKV	   *kvs;
	int			count;
	int			next_index;

	/*
	 * Per-fetch memory: every allocation produced while fetching a result set
	 * (the kvs array, decoded keys/values, response bodies) lives here and is
	 * reset before the next fetch.  Without this a parameterized nested-loop
	 * join would accumulate one fetch's worth of memory per outer tuple for
	 * the whole scan.
	 */
	MemoryContext fetch_cx;

	/* parameterized (join) scans: key computed from this expr at run time */
	ExprState  *param_expr;
	ExprContext *econtext;
	bool		fetched;

	int			prefix_len;
} EtcdScanState;

/* ----- helpers ----- */

static void
spec_to_request(EtcdScanSpec *spec, EtcdRangeRequest *req, bool count_only)
{
	MemSet(req, 0, sizeof(*req));
	req->key = spec->start;
	req->key_len = spec->start_len;
	if (!spec->single_key)
	{
		req->range_end = spec->end;
		req->range_end_len = spec->end_len;
	}
	req->count_only = count_only;
	req->sort_order = spec->sort_order;
	req->sort_target = ETCD_TARGET_KEY;
}

/* ----- planner callbacks ----- */

static void
etcdGetForeignRelSize(PlannerInfo *root, RelOptInfo *baserel,
					  Oid foreigntableid)
{
	EtcdRelInfo *ri = palloc0(sizeof(EtcdRelInfo));

	ri->opts = etcd_get_options(foreigntableid, GetUserId());
	etcd_deparse_analyze(root, baserel, foreigntableid, ri);
	baserel->fdw_private = ri;

	if (ri->spec->empty)
	{
		baserel->rows = 1;
	}
	else if (ri->opts->use_remote_estimate)
	{
		EtcdConn   *conn = etcd_conn_get(ri->opts);
		EtcdRangeRequest req;
		EtcdRangeResult *r;

		spec_to_request(ri->spec, &req, true);
		r = etcd_client_range(conn, &req);
		baserel->rows = Max(r->count_total, 1);
	}
	else
	{
		baserel->rows = ri->spec->single_key ? 1 : 1000;
	}
	baserel->tuples = baserel->rows;
}

/*
 * If the query's ORDER BY is a single key-column sort we can satisfy from
 * etcd's byte ordering, return the usable pathkeys and set *sort_order.
 */
static List *
get_usable_pathkeys(PlannerInfo *root, RelOptInfo *baserel, EtcdRelInfo *ri,
					EtcdSortOrder *sort_order)
{
	PathKey    *pk;
	EquivalenceClass *ec;
	ListCell   *lc;

	if (!ri->pushdown_safe_order)
		return NIL;
	if (list_length(root->query_pathkeys) != 1)
		return NIL;

	pk = (PathKey *) linitial(root->query_pathkeys);
	ec = pk->pk_eclass;

	if (!etcd_collation_is_byteordered(ec->ec_collation))
		return NIL;

	foreach(lc, ec->ec_members)
	{
		EquivalenceMember *em = (EquivalenceMember *) lfirst(lc);
		Expr	   *expr = em->em_expr;
		Var		   *var;

		while (expr && IsA(expr, RelabelType))
			expr = ((RelabelType *) expr)->arg;
		if (expr == NULL || !IsA(expr, Var))
			continue;
		var = (Var *) expr;
		if (var->varno == baserel->relid && var->varattno == ri->key_attno)
		{
			*sort_order = ETCD_PATHKEY_IS_DESC(pk)
				? ETCD_SORT_DESCEND : ETCD_SORT_ASCEND;
			return root->query_pathkeys;
		}
	}
	return NIL;
}

/*
 * If rinfo is "key = <expr-from-other-rels>" (deterministic-collation
 * equality), return the other-side expression (the run-time key source);
 * else NULL.
 */
static Expr *
key_eq_outer_expr(PlannerInfo *root, EtcdRelInfo *ri, RelOptInfo *baserel,
				  RestrictInfo *rinfo)
{
	OpExpr	   *op;
	char	   *opn;
	Node	   *l,
			   *r,
			   *ls,
			   *rs,
			   *other = NULL;
	Var		   *kv = NULL;

	if (ri->key_attno == 0)
		return NULL;
	if (!IsA(rinfo->clause, OpExpr))
		return NULL;
	op = (OpExpr *) rinfo->clause;
	if (list_length(op->args) != 2)
		return NULL;
	opn = get_opname(op->opno);
	if (opn == NULL || strcmp(opn, "=") != 0)
		return NULL;
	if (!etcd_collation_is_det_eq(ri->key_collation))
		return NULL;

	l = (Node *) linitial(op->args);
	r = (Node *) lsecond(op->args);
	ls = IsA(l, RelabelType) ? (Node *) ((RelabelType *) l)->arg : l;
	rs = IsA(r, RelabelType) ? (Node *) ((RelabelType *) r)->arg : r;

	if (IsA(ls, Var) && ((Var *) ls)->varno == baserel->relid &&
		((Var *) ls)->varattno == ri->key_attno)
	{
		kv = (Var *) ls;
		other = r;
	}
	else if (IsA(rs, Var) && ((Var *) rs)->varno == baserel->relid &&
			 ((Var *) rs)->varattno == ri->key_attno)
	{
		kv = (Var *) rs;
		other = l;
	}
	if (kv == NULL)
		return NULL;

	/* the other side must not reference our own relation */
	if (bms_is_member(baserel->relid, pull_varnos(root, other)))
		return NULL;

	return (Expr *) other;
}

static void
etcdGetForeignPaths(PlannerInfo *root, RelOptInfo *baserel,
					Oid foreigntableid)
{
	EtcdRelInfo *ri = (EtcdRelInfo *) baserel->fdw_private;
	Cost		startup_cost = 100.0;
	Cost		total_cost;
	List	   *usable_pathkeys;
	EtcdSortOrder sort_order = ETCD_SORT_ASCEND;

	total_cost = startup_cost + baserel->rows;

	/* LIMIT pushdown: only when nothing is rechecked locally. */
	if (ri->local_conds == NIL && root->limit_tuples > 0 &&
		!ri->spec->empty)
	{
		/* safe whether or not we push ORDER BY: a non-ordered scan with no
		 * local filtering may return any N matching rows. */
		ri->spec->limit = (int64) ceil(root->limit_tuples);
	}

	/* plain (unordered) path */
	add_path(baserel, (Path *)
			 ETCD_CREATE_FOREIGNSCAN_PATH(root, baserel, NULL, baserel->rows,
										  startup_cost, total_cost,
										  NIL, NULL, NULL, NIL));

	/* ordered path, if ORDER BY key is satisfiable from etcd byte ordering
	 * (not for multi-key IN scans, which we do not return in sorted order) */
	if (ri->spec->nkeys == 0)
	{
		usable_pathkeys = get_usable_pathkeys(root, baserel, ri, &sort_order);
		if (usable_pathkeys != NIL)
		{
			ri->spec->sort_order = sort_order;
			add_path(baserel, (Path *)
					 ETCD_CREATE_FOREIGNSCAN_PATH(root, baserel, NULL, baserel->rows,
												  startup_cost, total_cost - 1,
												  usable_pathkeys, NULL, NULL, NIL));
		}
	}

	/*
	 * Parameterized paths: for each "key = <outer expr>" relationship, offer a
	 * single-key lookup parameterized by the outer relation(s), so a nested
	 * loop fetches one etcd key per outer row instead of scanning the prefix.
	 * Such equalities usually live in equivalence classes (mergejoinable "="),
	 * so we collect candidate outer relid sets from both joininfo and ECs.
	 */
	if (ri->key_attno != 0)
	{
		List	   *cand = NIL; /* list of candidate required_outer Relids */
		ListCell   *jc;

		/* explicit (non-EC) join clauses */
		foreach(jc, baserel->joininfo)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, jc);
			Relids		req;

			if (key_eq_outer_expr(root, ri, baserel, rinfo) == NULL)
				continue;
			req = bms_difference(rinfo->clause_relids, baserel->relids);
			if (!bms_is_empty(req))
				cand = lappend(cand, req);
		}

		/* EC-derived equalities involving the key column */
		foreach(jc, root->eq_classes)
		{
			EquivalenceClass *ec = (EquivalenceClass *) lfirst(jc);
			ListCell   *m;
			bool		has_key = false;

			if (!etcd_collation_is_det_eq(ec->ec_collation))
				continue;
			foreach(m, ec->ec_members)
			{
				EquivalenceMember *em = (EquivalenceMember *) lfirst(m);
				Expr	   *e = em->em_expr;

				while (e && IsA(e, RelabelType))
					e = ((RelabelType *) e)->arg;
				if (e && IsA(e, Var) &&
					((Var *) e)->varno == baserel->relid &&
					((Var *) e)->varattno == ri->key_attno)
				{
					has_key = true;
					break;
				}
			}
			if (!has_key)
				continue;

			foreach(m, ec->ec_members)
			{
				EquivalenceMember *em = (EquivalenceMember *) lfirst(m);

				if (em->em_is_const ||
					bms_is_empty(em->em_relids) ||
					bms_overlap(em->em_relids, baserel->relids))
					continue;
				cand = lappend(cand, bms_copy(em->em_relids));
			}
		}

		/* add one parameterized path per distinct required_outer set */
		{
			List	   *added = NIL;

			foreach(jc, cand)
			{
				Relids		req = (Relids) lfirst(jc);
				ListCell   *p;
				bool		dup = false;

				foreach(p, added)
					if (bms_equal((Relids) lfirst(p), req))
					{
						dup = true;
						break;
					}
				if (dup)
					continue;
				added = lappend(added, req);

				add_path(baserel, (Path *)
						 ETCD_CREATE_FOREIGNSCAN_PATH(root, baserel, NULL, 1,
													  startup_cost, startup_cost + 1,
													  NIL, req, NULL, NIL));
			}
		}
	}
}

static ForeignScan *
etcdGetForeignPlan(PlannerInfo *root, RelOptInfo *baserel,
				   Oid foreigntableid, ForeignPath *best_path,
				   List *tlist, List *scan_clauses, Plan *outer_plan)
{
	EtcdRelInfo *ri = (EtcdRelInfo *) baserel->fdw_private;
	Index		scan_relid = baserel->relid;
	List	   *local_exprs = NIL;
	List	   *fdw_exprs = NIL;
	List	   *fdw_private;
	ListCell   *lc;

	if (best_path->path.param_info != NULL)
	{
		/*
		 * Parameterized scan: find the "key = <outer expr>" clause, drive the
		 * lookup from it, and recheck every other clause locally (param mode
		 * ignores the analyze-time range, so we must not drop those).
		 */
		Expr	   *param_expr = NULL;

		foreach(lc, scan_clauses)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);
			Expr	   *oexpr;

			oexpr = (param_expr == NULL)
				? key_eq_outer_expr(root, ri, baserel, rinfo) : NULL;
			if (oexpr != NULL)
				param_expr = oexpr;		/* handled via the param lookup */
			else
				local_exprs = lappend(local_exprs, rinfo->clause);
		}

		if (param_expr != NULL)
		{
			ri->spec->param_key = true;
			fdw_exprs = list_make1(copyObject(param_expr));
		}
	}
	else
	{
		/* Keep only the clauses etcd did not handle for local recheck. */
		foreach(lc, scan_clauses)
		{
			RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

			if (list_member_ptr(ri->remote_conds, rinfo))
				continue;
			local_exprs = lappend(local_exprs, rinfo->clause);
		}
	}

	fdw_private = etcd_serialize_spec(ri->spec);

	return make_foreignscan(tlist,
							local_exprs,
							scan_relid,
							fdw_exprs,
							fdw_private,
							NIL,
							NIL,
							outer_plan);
}

/* ----- execution callbacks ----- */

static void
build_colmap(EtcdScanState *st, Relation rel)
{
	TupleDesc	td = RelationGetDescr(rel);
	int			i;

	st->ncols = td->natts;
	st->colkind = palloc0(sizeof(EtcdColKind) * td->natts);
	st->coltype = palloc0(sizeof(Oid) * td->natts);

	for (i = 0; i < td->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(td, i);
		const char *name;

		if (attr->attisdropped)
			continue;
		name = NameStr(attr->attname);
		st->coltype[i] = attr->atttypid;

		if (strcmp(name, "key") == 0)
			st->colkind[i] = ETCD_COL_KEY;
		else if (strcmp(name, "value") == 0)
			st->colkind[i] = ETCD_COL_VALUE;
		else if (strcmp(name, "create_revision") == 0)
			st->colkind[i] = ETCD_COL_CREATE_REVISION;
		else if (strcmp(name, "mod_revision") == 0)
			st->colkind[i] = ETCD_COL_MOD_REVISION;
		else if (strcmp(name, "version") == 0)
			st->colkind[i] = ETCD_COL_VERSION;
		else if (strcmp(name, "lease") == 0)
			st->colkind[i] = ETCD_COL_LEASE;
		else
			st->colkind[i] = ETCD_COL_NONE;
	}
}

/* grow st->kvs and append one range result */
static void
append_result(EtcdScanState *st, int *cap, EtcdRangeResult *r)
{
	int			i;

	if (st->count + r->count > *cap)
	{
		*cap = Max(*cap * 2, st->count + r->count);
		st->kvs = st->kvs
			? repalloc(st->kvs, sizeof(EtcdKV) * (*cap))
			: palloc(sizeof(EtcdKV) * (*cap));
	}
	for (i = 0; i < r->count; i++)
		st->kvs[st->count++] = r->kvs[i];
}

/* build the full etcd key from a user-facing key value */
static char *
scan_full_key(EtcdScanState *st, const char *uk, int ukl, int *outlen)
{
	if (st->opts->strip_prefix && st->prefix_len > 0)
	{
		char	   *r = palloc(st->prefix_len + ukl);

		memcpy(r, st->opts->prefix, st->prefix_len);
		memcpy(r + st->prefix_len, uk, ukl);
		*outlen = st->prefix_len + ukl;
		return r;
	}
	*outlen = ukl;
	return pnstrdup(uk, ukl);
}

/* fetch exactly one key and append it */
static void
fetch_single(EtcdScanState *st, int *cap, const char *key, int keylen)
{
	EtcdRangeRequest req;

	MemSet(&req, 0, sizeof(req));
	req.key = key;
	req.key_len = keylen;
	append_result(st, cap, etcd_client_range(st->conn, &req));
}

static void
execute_scan(EtcdScanState *st)
{
	EtcdScanSpec *spec = st->spec;
	int			cap = 0;
	char	   *start = spec->start;
	int			start_len = spec->start_len;
	int64		remaining = spec->limit;
	MemoryContext old;

	/*
	 * Discard the previous fetch and run this one entirely inside fetch_cx, so
	 * the result set (kvs array, decoded keys/values, response bodies) is
	 * reclaimed here on the next fetch instead of accumulating for the life of
	 * the scan.  Tuples for the prior fetch have already been emitted and
	 * copied out by the executor before a rescan triggers the next fetch.
	 */
	MemoryContextReset(st->fetch_cx);
	st->kvs = NULL;
	st->count = 0;

	if (spec->empty)
		return;

	old = MemoryContextSwitchTo(st->fetch_cx);

	/* parameterized join: evaluate the key expression for this outer tuple */
	if (spec->param_key)
	{
		Datum		d;
		bool		isnull;
		struct varlena *vl;
		char	   *fk;
		int			fklen;

		/*
		 * The parameter expression must be evaluated against the executor's
		 * own context (it owns the outer tuple), not fetch_cx.
		 */
		if (st->param_expr == NULL)
		{
			MemoryContextSwitchTo(old);
			return;
		}
		MemoryContextSwitchTo(old);
		d = ExecEvalExpr(st->param_expr, st->econtext, &isnull);
		if (isnull)
			return;				/* NULL never matches a key */
		MemoryContextSwitchTo(st->fetch_cx);
		vl = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));
		fk = scan_full_key(st, VARDATA_ANY(vl), VARSIZE_ANY_EXHDR(vl), &fklen);
		fetch_single(st, &cap, fk, fklen);
		MemoryContextSwitchTo(old);
		return;
	}

	/* "key IN (...)" : one get per requested key */
	if (spec->nkeys > 0)
	{
		int			i;

		for (i = 0; i < spec->nkeys; i++)
			fetch_single(st, &cap, spec->keys[i], spec->keylens[i]);
		MemoryContextSwitchTo(old);
		return;
	}

	/*
	 * A whole-keyspace scan has an empty start key, but etcd's Range rejects an
	 * empty key; its convention is key == range_end == "\0" to mean "all keys".
	 */
	if (start_len == 0)
	{
		start = palloc(1);
		start[0] = '\0';
		start_len = 1;
	}

	/* single key or range, following etcd "more" for large ascending scans */
	for (;;)
	{
		EtcdRangeRequest req;
		EtcdRangeResult *r;

		spec_to_request(spec, &req, false);
		req.key = start;
		req.key_len = start_len;
		if (remaining > 0)
			req.limit = remaining;

		r = etcd_client_range(st->conn, &req);
		append_result(st, &cap, r);

		if (spec->single_key)
			break;
		if (remaining > 0)
		{
			remaining -= r->count;
			if (remaining <= 0)
				break;
		}
		/* continuation only makes sense for ascending unbounded scans */
		if (!r->more || spec->sort_order != ETCD_SORT_ASCEND)
			break;
		if (r->count == 0)
			break;

		/* resume strictly after the last returned key */
		{
			EtcdKV	   *last = &r->kvs[r->count - 1];

			start = palloc(last->key_len + 1);
			memcpy(start, last->key, last->key_len);
			start[last->key_len] = '\0';
			start_len = last->key_len + 1;
		}
	}

	MemoryContextSwitchTo(old);
}

static void
etcdBeginForeignScan(ForeignScanState *node, int eflags)
{
	EtcdScanState *st;
	Relation	rel = node->ss.ss_currentRelation;
	Oid			relid = RelationGetRelid(rel);
	ForeignScan *fsplan = (ForeignScan *) node->ss.ps.plan;

	st = palloc0(sizeof(EtcdScanState));
	st->opts = etcd_get_options(relid, GetUserId());
	st->conn = etcd_conn_get(st->opts);
	st->prefix_len = (int) strlen(st->opts->prefix);
	st->spec = etcd_deserialize_spec(fsplan->fdw_private);
	build_colmap(st, rel);
	st->fetch_cx = AllocSetContextCreate(CurrentMemoryContext,
										 "etcd_fdw fetch",
										 ALLOCSET_DEFAULT_SIZES);
	node->fdw_state = st;

	/* parameterized join: prepare the run-time key expression */
	if (st->spec->param_key && fsplan->fdw_exprs != NIL)
	{
		st->econtext = node->ss.ps.ps_ExprContext;
		st->param_expr = ExecInitExpr((Expr *) linitial(fsplan->fdw_exprs),
									  (PlanState *) node);
	}

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	st->fetched = false;
	if (!st->spec->param_key)
	{
		execute_scan(st);
		st->fetched = true;
	}
	st->next_index = 0;
}

static Datum
make_value_datum(EtcdScanState *st, int col, EtcdKV *kv, bool *isnull)
{
	if (kv->value == NULL)
	{
		*isnull = true;
		return (Datum) 0;
	}
	*isnull = false;

	if (st->coltype[col] == BYTEAOID)
	{
		bytea	   *b = (bytea *) palloc(VARHDRSZ + kv->value_len);

		SET_VARSIZE(b, VARHDRSZ + kv->value_len);
		memcpy(VARDATA(b), kv->value, kv->value_len);
		return PointerGetDatum(b);
	}
	return PointerGetDatum(cstring_to_text_with_len(kv->value, kv->value_len));
}

static TupleTableSlot *
etcdIterateForeignScan(ForeignScanState *node)
{
	EtcdScanState *st = (EtcdScanState *) node->fdw_state;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	EtcdKV	   *kv;
	int			i;

	/* parameterized scans fetch lazily so each outer tuple re-queries etcd */
	if (!st->fetched)
	{
		execute_scan(st);
		st->fetched = true;
		st->next_index = 0;
	}

	ExecClearTuple(slot);

	if (st->next_index >= st->count)
		return slot;

	kv = &st->kvs[st->next_index++];

	for (i = 0; i < st->ncols; i++)
	{
		Datum		d = (Datum) 0;
		bool		isnull = true;

		switch (st->colkind[i])
		{
			case ETCD_COL_KEY:
				{
					int			off = 0;
					int			len = kv->key_len;

					if (st->opts->strip_prefix && len >= st->prefix_len)
					{
						off = st->prefix_len;
						len -= st->prefix_len;
					}
					d = PointerGetDatum(cstring_to_text_with_len(kv->key + off, len));
					isnull = false;
					break;
				}
			case ETCD_COL_VALUE:
				d = make_value_datum(st, i, kv, &isnull);
				break;
			case ETCD_COL_CREATE_REVISION:
				d = Int64GetDatum(kv->create_revision);
				isnull = false;
				break;
			case ETCD_COL_MOD_REVISION:
				d = Int64GetDatum(kv->mod_revision);
				isnull = false;
				break;
			case ETCD_COL_VERSION:
				d = Int64GetDatum(kv->version);
				isnull = false;
				break;
			case ETCD_COL_LEASE:
				d = Int64GetDatum(kv->lease);
				isnull = false;
				break;
			case ETCD_COL_NONE:
			default:
				isnull = true;
				break;
		}
		slot->tts_values[i] = d;
		slot->tts_isnull[i] = isnull;
	}

	ExecStoreVirtualTuple(slot);
	return slot;
}

static void
etcdReScanForeignScan(ForeignScanState *node)
{
	EtcdScanState *st = (EtcdScanState *) node->fdw_state;

	/* parameterized scans must re-fetch with the new outer-tuple key */
	if (st->spec->param_key)
		st->fetched = false;
	st->next_index = 0;
}

static void
etcdEndForeignScan(ForeignScanState *node)
{
	EtcdScanState *st = (EtcdScanState *) node->fdw_state;

	/* Connections are cached and reused; only the per-fetch arena is ours. */
	if (st && st->fetch_cx)
	{
		MemoryContextDelete(st->fetch_cx);
		st->fetch_cx = NULL;
	}
}

static void
etcdExplainForeignScan(ForeignScanState *node, ExplainState *es)
{
	EtcdScanState *st = (EtcdScanState *) node->fdw_state;

	if (st && st->opts)
	{
		const char *base = (char *) linitial(st->opts->endpoints);

		ExplainPropertyText("etcd Endpoint", base, es);
		ExplainPropertyText("etcd Prefix",
							st->prefix_len > 0 ? st->opts->prefix
							: "(whole keyspace)", es);
		if (st->spec)
		{
			if (st->spec->empty)
				ExplainPropertyText("etcd Scan", "(provably empty)", es);
			else if (st->spec->param_key)
				ExplainPropertyText("etcd Scan", "parameterized key", es);
			else if (st->spec->nkeys > 0)
				ExplainPropertyInteger("etcd Scan keys", NULL,
									   st->spec->nkeys, es);
			else if (st->spec->single_key)
				ExplainPropertyText("etcd Scan", "single key", es);
			else
				ExplainPropertyText("etcd Scan", "range", es);
			if (st->spec->limit > 0)
				ExplainPropertyInteger("etcd Limit", NULL, st->spec->limit, es);
			ExplainPropertyText("etcd Order",
								st->spec->sort_order == ETCD_SORT_DESCEND
								? "DESC" : "ASC", es);
		}
	}
}

/* ----- modify (INSERT/UPDATE/DELETE) callbacks ----- */

typedef struct EtcdModifyState
{
	EtcdFdwOptions *opts;
	EtcdConn   *conn;
	int			prefix_len;

	AttrNumber	key_attno;		/* relation attnums (1-based), 0 if absent */
	AttrNumber	value_attno;
	AttrNumber	lease_attno;
	Oid			value_type;

	AttrNumber	junk_key;		/* planSlot junk attnos for row identity */
	AttrNumber	junk_modrev;

	/*
	 * Per-row scratch: each Exec callback resets this and works inside it, so a
	 * bulk INSERT/UPDATE/DELETE or COPY does not pile up one row's worth of
	 * key/value copies, base64 buffers and request/response trees in the
	 * long-lived per-query context for the whole statement.
	 */
	MemoryContext temp_cx;
} EtcdModifyState;

/* find a column's attnum (1-based) by name, 0 if missing; sets *typid */
static AttrNumber
find_attr(Relation rel, const char *name, Oid *typid)
{
	TupleDesc	td = RelationGetDescr(rel);
	int			i;

	for (i = 0; i < td->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(td, i);

		if (!attr->attisdropped && strcmp(NameStr(attr->attname), name) == 0)
		{
			if (typid)
				*typid = attr->atttypid;
			return attr->attnum;
		}
	}
	if (typid)
		*typid = InvalidOid;
	return 0;
}

/* extract raw bytes from a varlena slot attribute (text or bytea) */
static char *
attr_bytes(TupleTableSlot *slot, AttrNumber attno, int *len, bool *isnull)
{
	Datum		d = slot_getattr(slot, attno, isnull);
	struct varlena *vl;
	char	   *r;

	if (*isnull)
	{
		*len = 0;
		return NULL;
	}
	vl = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));
	*len = VARSIZE_ANY_EXHDR(vl);
	r = palloc(*len + 1);
	memcpy(r, VARDATA_ANY(vl), *len);
	r[*len] = '\0';
	return r;
}

/* full etcd key from a user-facing key value */
static char *
full_key(EtcdModifyState *st, const char *userkey, int userlen, int *outlen)
{
	if (st->opts->strip_prefix && st->prefix_len > 0)
	{
		char	   *r = palloc(st->prefix_len + userlen);

		memcpy(r, st->opts->prefix, st->prefix_len);
		memcpy(r + st->prefix_len, userkey, userlen);
		*outlen = st->prefix_len + userlen;
		return r;
	}
	*outlen = userlen;
	return pnstrdup(userkey, userlen);
}

static void
etcdAddForeignUpdateTargets(PlannerInfo *root, Index rtindex,
							RangeTblEntry *target_rte, Relation target_relation)
{
	Oid			typid;
	AttrNumber	attno;
	Var		   *var;

	/* row identity: the key column */
	attno = find_attr(target_relation, "key", &typid);
	if (attno == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("etcd_fdw: foreign table \"%s\" must have a \"key\" column to be updatable",
						RelationGetRelationName(target_relation))));
	var = makeVar(rtindex, attno, typid, -1,
				  TupleDescAttr(RelationGetDescr(target_relation), attno - 1)->attcollation,
				  0);
	add_row_identity_var(root, var, rtindex, "etcd_key");

	/* optimistic-concurrency guard: mod_revision, if present */
	attno = find_attr(target_relation, "mod_revision", &typid);
	if (attno != 0)
	{
		var = makeVar(rtindex, attno, typid, -1, InvalidOid, 0);
		add_row_identity_var(root, var, rtindex, "etcd_modrev");
	}
}

static List *
etcdPlanForeignModify(PlannerInfo *root, ModifyTable *plan,
					  Index resultRelation, int subplan_index)
{
	/* Everything is resolved at BeginForeignModify; nothing to stash. */
	return NIL;
}

static void
etcdBeginForeignModify(ModifyTableState *mtstate, ResultRelInfo *rinfo,
					   List *fdw_private, int subplan_index, int eflags)
{
	EtcdModifyState *st = palloc0(sizeof(EtcdModifyState));
	Relation	rel = rinfo->ri_RelationDesc;
	Plan	   *subplan = outerPlanState(mtstate)->plan;

	st->opts = etcd_get_options(RelationGetRelid(rel), GetUserId());
	st->conn = etcd_conn_get(st->opts);
	st->prefix_len = (int) strlen(st->opts->prefix);

	st->key_attno = find_attr(rel, "key", NULL);
	st->value_attno = find_attr(rel, "value", &st->value_type);
	st->lease_attno = find_attr(rel, "lease", NULL);

	st->junk_key = ExecFindJunkAttributeInTlist(subplan->targetlist, "etcd_key");
	st->junk_modrev = ExecFindJunkAttributeInTlist(subplan->targetlist, "etcd_modrev");

	st->temp_cx = AllocSetContextCreate(CurrentMemoryContext,
										"etcd_fdw modify",
										ALLOCSET_SMALL_SIZES);

	rinfo->ri_FdwState = st;

	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;
}

/* read key/value/lease from a (new) tuple slot */
static void
read_new_tuple(EtcdModifyState *st, TupleTableSlot *slot,
			   char **fkey, int *fkeylen,
			   char **value, int *vallen, bool *value_isnull,
			   int64 *lease)
{
	bool		isnull;
	char	   *ukey;
	int			ukeylen;

	slot_getallattrs(slot);

	if (st->key_attno == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("etcd_fdw: table has no \"key\" column")));
	ukey = attr_bytes(slot, st->key_attno, &ukeylen, &isnull);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_NOT_NULL_VIOLATION),
				 errmsg("etcd_fdw: \"key\" must not be NULL")));
	*fkey = full_key(st, ukey, ukeylen, fkeylen);

	*value = NULL;
	*vallen = 0;
	*value_isnull = true;
	if (st->value_attno != 0)
		*value = attr_bytes(slot, st->value_attno, vallen, value_isnull);

	*lease = 0;
	if (st->lease_attno != 0)
	{
		Datum		d = slot_getattr(slot, st->lease_attno, &isnull);

		if (!isnull)
			*lease = DatumGetInt64(d);
	}
}

static TupleTableSlot *
etcdExecForeignInsert(EState *estate, ResultRelInfo *rinfo,
					  TupleTableSlot *slot, TupleTableSlot *planSlot)
{
	EtcdModifyState *st = (EtcdModifyState *) rinfo->ri_FdwState;
	char	   *fkey;
	int			fkeylen;
	char	   *value;
	int			vallen;
	bool		value_isnull;
	int64		lease;
	MemoryContext old;

	MemoryContextReset(st->temp_cx);
	old = MemoryContextSwitchTo(st->temp_cx);

	read_new_tuple(st, slot, &fkey, &fkeylen, &value, &vallen,
				   &value_isnull, &lease);

	etcd_client_put(st->conn, fkey, fkeylen,
					value_isnull ? "" : value, value_isnull ? 0 : vallen,
					lease);

	MemoryContextSwitchTo(old);
	return slot;
}

/* etcd's default --max-txn-ops is 128; stay safely under it. */
#define ETCD_INSERT_BATCH_SIZE 100

static int
etcdGetForeignModifyBatchSize(ResultRelInfo *rinfo)
{
	return ETCD_INSERT_BATCH_SIZE;
}

static TupleTableSlot **
etcdExecForeignBatchInsert(EState *estate, ResultRelInfo *rinfo,
						   TupleTableSlot **slots, TupleTableSlot **planSlots,
						   int *numSlots)
{
	EtcdModifyState *st = (EtcdModifyState *) rinfo->ri_FdwState;
	int			n = *numSlots;
	EtcdOp	   *ops;
	int			i;
	MemoryContext old;

	MemoryContextReset(st->temp_cx);
	old = MemoryContextSwitchTo(st->temp_cx);

	ops = palloc(sizeof(EtcdOp) * n);

	for (i = 0; i < n; i++)
	{
		char	   *fkey;
		int			fkeylen;
		char	   *value;
		int			vallen;
		bool		value_isnull;
		int64		lease;

		read_new_tuple(st, slots[i], &fkey, &fkeylen, &value, &vallen,
					   &value_isnull, &lease);
		ops[i].type = ETCD_OP_PUT;
		ops[i].key = fkey;
		ops[i].key_len = fkeylen;
		ops[i].value = value_isnull ? "" : value;
		ops[i].value_len = value_isnull ? 0 : vallen;
		ops[i].lease = lease;
	}

	/* one etcd transaction applies all puts atomically */
	etcd_client_txn(st->conn, NULL, 0, ops, n);

	MemoryContextSwitchTo(old);
	return slots;
}

/* fetch the old row-identity key (full etcd key) and mod_revision from planSlot */
static void
read_old_identity(EtcdModifyState *st, TupleTableSlot *planSlot,
				  char **oldkey, int *oldkeylen,
				  int64 *modrev, bool *have_modrev)
{
	bool		isnull;
	Datum		d;
	struct varlena *vl;
	char	   *ukey;
	int			ukeylen;

	if (st->junk_key == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("etcd_fdw: missing row identity for modify")));

	d = ExecGetJunkAttribute(planSlot, st->junk_key, &isnull);
	if (isnull)
		ereport(ERROR,
				(errcode(ERRCODE_FDW_ERROR),
				 errmsg("etcd_fdw: row identity \"key\" is NULL")));
	vl = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));
	ukeylen = VARSIZE_ANY_EXHDR(vl);
	ukey = pnstrdup(VARDATA_ANY(vl), ukeylen);
	*oldkey = full_key(st, ukey, ukeylen, oldkeylen);

	*have_modrev = false;
	*modrev = 0;
	if (st->junk_modrev != InvalidAttrNumber)
	{
		d = ExecGetJunkAttribute(planSlot, st->junk_modrev, &isnull);
		if (!isnull)
		{
			*modrev = DatumGetInt64(d);
			*have_modrev = true;
		}
	}
}

static void
concurrent_modify_error(void)
{
	ereport(ERROR,
			(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
			 errmsg("etcd_fdw: row was modified concurrently in etcd"),
			 errhint("The key's mod_revision changed since it was read.")));
}

static TupleTableSlot *
etcdExecForeignUpdate(EState *estate, ResultRelInfo *rinfo,
					  TupleTableSlot *slot, TupleTableSlot *planSlot)
{
	EtcdModifyState *st = (EtcdModifyState *) rinfo->ri_FdwState;
	char	   *oldkey;
	int			oldkeylen;
	int64		modrev;
	bool		have_modrev;
	char	   *newkey;
	int			newkeylen;
	char	   *value;
	int			vallen;
	bool		value_isnull;
	int64		lease;
	bool		key_changed;
	MemoryContext old;

	MemoryContextReset(st->temp_cx);
	old = MemoryContextSwitchTo(st->temp_cx);

	read_old_identity(st, planSlot, &oldkey, &oldkeylen, &modrev, &have_modrev);
	read_new_tuple(st, slot, &newkey, &newkeylen, &value, &vallen,
				   &value_isnull, &lease);

	key_changed = !(oldkeylen == newkeylen &&
					memcmp(oldkey, newkey, oldkeylen) == 0);

	if (have_modrev)
	{
		EtcdCompare cmp;
		EtcdOp		ops[2];
		int			nops = 0;

		cmp.key = oldkey;
		cmp.key_len = oldkeylen;
		cmp.mod_revision = modrev;

		ops[nops].type = ETCD_OP_PUT;
		ops[nops].key = newkey;
		ops[nops].key_len = newkeylen;
		ops[nops].value = value_isnull ? "" : value;
		ops[nops].value_len = value_isnull ? 0 : vallen;
		ops[nops].lease = lease;
		nops++;
		if (key_changed)
		{
			ops[nops].type = ETCD_OP_DELETE;
			ops[nops].key = oldkey;
			ops[nops].key_len = oldkeylen;
			ops[nops].value = NULL;
			ops[nops].value_len = 0;
			ops[nops].lease = 0;
			nops++;
		}
		if (!etcd_client_txn(st->conn, &cmp, 1, ops, nops))
			concurrent_modify_error();
	}
	else
	{
		etcd_client_put(st->conn, newkey, newkeylen,
						value_isnull ? "" : value, value_isnull ? 0 : vallen,
						lease);
		if (key_changed)
			etcd_client_delete_range(st->conn, oldkey, oldkeylen, NULL, 0);
	}

	MemoryContextSwitchTo(old);
	return slot;
}

static TupleTableSlot *
etcdExecForeignDelete(EState *estate, ResultRelInfo *rinfo,
					  TupleTableSlot *slot, TupleTableSlot *planSlot)
{
	EtcdModifyState *st = (EtcdModifyState *) rinfo->ri_FdwState;
	char	   *oldkey;
	int			oldkeylen;
	int64		modrev;
	bool		have_modrev;
	MemoryContext old;

	MemoryContextReset(st->temp_cx);
	old = MemoryContextSwitchTo(st->temp_cx);

	read_old_identity(st, planSlot, &oldkey, &oldkeylen, &modrev, &have_modrev);

	if (have_modrev)
	{
		EtcdCompare cmp;
		EtcdOp		op;

		cmp.key = oldkey;
		cmp.key_len = oldkeylen;
		cmp.mod_revision = modrev;
		op.type = ETCD_OP_DELETE;
		op.key = oldkey;
		op.key_len = oldkeylen;
		op.value = NULL;
		op.value_len = 0;
		op.lease = 0;
		if (!etcd_client_txn(st->conn, &cmp, 1, &op, 1))
			concurrent_modify_error();
	}
	else
	{
		etcd_client_delete_range(st->conn, oldkey, oldkeylen, NULL, 0);
	}

	MemoryContextSwitchTo(old);
	return slot;
}

static void
etcdEndForeignModify(EState *estate, ResultRelInfo *rinfo)
{
	EtcdModifyState *st = (EtcdModifyState *) rinfo->ri_FdwState;

	/* connection is cached; only the per-row scratch context is ours */
	if (st && st->temp_cx)
	{
		MemoryContextDelete(st->temp_cx);
		st->temp_cx = NULL;
	}
}

/*
 * BeginForeignInsert / EndForeignInsert handle direct-insert paths that do not
 * go through PlanForeignModify, notably COPY FROM and partition tuple routing.
 */
static void
etcdBeginForeignInsert(ModifyTableState *mtstate, ResultRelInfo *rinfo)
{
	EtcdModifyState *st;
	Relation	rel = rinfo->ri_RelationDesc;

	/* already set up by BeginForeignModify? reuse it */
	if (rinfo->ri_FdwState != NULL)
		return;

	st = palloc0(sizeof(EtcdModifyState));
	st->opts = etcd_get_options(RelationGetRelid(rel), GetUserId());
	st->conn = etcd_conn_get(st->opts);
	st->prefix_len = (int) strlen(st->opts->prefix);
	st->key_attno = find_attr(rel, "key", NULL);
	st->value_attno = find_attr(rel, "value", &st->value_type);
	st->lease_attno = find_attr(rel, "lease", NULL);
	st->junk_key = InvalidAttrNumber;
	st->junk_modrev = InvalidAttrNumber;
	st->temp_cx = AllocSetContextCreate(CurrentMemoryContext,
										"etcd_fdw modify",
										ALLOCSET_SMALL_SIZES);
	rinfo->ri_FdwState = st;
}

static void
etcdEndForeignInsert(EState *estate, ResultRelInfo *rinfo)
{
	EtcdModifyState *st = (EtcdModifyState *) rinfo->ri_FdwState;

	/* connection is cached; only the per-row scratch context is ours */
	if (st && st->temp_cx)
	{
		MemoryContextDelete(st->temp_cx);
		st->temp_cx = NULL;
	}
}

static int
etcdIsForeignRelUpdatable(Relation rel)
{
	/* updatable only if a key column exists */
	if (find_attr(rel, "key", NULL) == 0)
		return 0;
	return (1 << CMD_INSERT) | (1 << CMD_UPDATE) | (1 << CMD_DELETE);
}

/* TRUNCATE: delete the whole prefix range of each target table. */
static void
etcdExecForeignTruncate(List *rels, DropBehavior behavior, bool restart_seqs)
{
	ListCell   *lc;

	foreach(lc, rels)
	{
		Relation	rel = (Relation) lfirst(lc);
		EtcdFdwOptions *opts = etcd_get_options(RelationGetRelid(rel), GetUserId());
		EtcdConn   *conn = etcd_conn_get(opts);
		int			plen = (int) strlen(opts->prefix);
		int			endlen;
		char	   *end = etcd_prefix_range_end(opts->prefix, plen, &endlen);

		etcd_client_delete_range(conn, opts->prefix, plen, end, endlen);
	}
}

/* ----- IMPORT FOREIGN SCHEMA ----- */

/* sanitise an etcd path segment into a SQL identifier */
static char *
sanitize_ident(const char *seg, int len)
{
	StringInfoData s;
	int			i;

	initStringInfo(&s);
	for (i = 0; i < len; i++)
	{
		char		c = seg[i];

		if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')
			appendStringInfoChar(&s, c);
		else if (c >= 'A' && c <= 'Z')
			appendStringInfoChar(&s, c - 'A' + 'a');
		else
			appendStringInfoChar(&s, '_');
	}
	if (s.len == 0)
		appendStringInfoString(&s, "etcd");
	return s.data;
}

static List *
etcdImportForeignSchema(ImportForeignSchemaStmt *stmt, Oid serverOid)
{
	List	   *commands = NIL;
	EtcdFdwOptions *opts;
	EtcdConn   *conn;
	EtcdRangeRequest req;
	EtcdRangeResult *r;
	const char *base_prefix;
	int			base_len;
	char	   *range_end;
	int			range_end_len;
	List	   *dirs = NIL;		/* discovered child directory segments */
	ListCell   *lc;
	int			i;

	/* remote_schema is interpreted as an etcd key prefix */
	base_prefix = stmt->remote_schema;
	if (base_prefix == NULL)
		base_prefix = "";
	base_len = (int) strlen(base_prefix);

	opts = etcd_get_server_options(serverOid, GetUserId(), base_prefix);
	conn = etcd_conn_get(opts);

	/* list all keys under the prefix (keys only) */
	MemSet(&req, 0, sizeof(req));
	req.key = base_prefix;
	req.key_len = base_len;
	range_end = etcd_prefix_range_end(base_prefix, base_len, &range_end_len);
	req.range_end = range_end;
	req.range_end_len = range_end_len;
	req.keys_only = true;
	req.sort_order = ETCD_SORT_ASCEND;
	req.sort_target = ETCD_TARGET_KEY;
	r = etcd_client_range(conn, &req);

	/* collect immediate child "directory" segments (split on '/') */
	for (i = 0; i < r->count; i++)
	{
		EtcdKV	   *kv = &r->kvs[i];
		const char *rem = kv->key + base_len;
		int			remlen = kv->key_len - base_len;
		int			j;

		if (remlen <= 0)
			continue;
		for (j = 0; j < remlen; j++)
			if (rem[j] == '/')
				break;
		if (j < remlen)
		{
			/* rem[0..j) is a child directory segment */
			char	   *seg = pnstrdup(rem, j);
			bool		seen = false;
			ListCell   *l2;

			foreach(l2, dirs)
				if (strcmp((char *) lfirst(l2), seg) == 0)
				{
					seen = true;
					break;
				}
			if (!seen)
				dirs = lappend(dirs, seg);
		}
	}

	/* emit one foreign table per discovered child directory */
	foreach(lc, dirs)
	{
		char	   *seg = (char *) lfirst(lc);
		char	   *tabname = sanitize_ident(seg, strlen(seg));
		StringInfoData cmd;

		initStringInfo(&cmd);
		appendStringInfo(&cmd,
						 "CREATE FOREIGN TABLE %s (\n"
						 "  key text COLLATE \"C\",\n"
						 "  value text,\n"
						 "  create_revision bigint,\n"
						 "  mod_revision bigint,\n"
						 "  version bigint,\n"
						 "  lease bigint\n"
						 ") SERVER %s OPTIONS (prefix %s, strip_prefix 'true')",
						 quote_identifier(tabname),
						 quote_identifier(stmt->server_name),
						 quote_literal_cstr(psprintf("%s%s/", base_prefix, seg)));
		commands = lappend(commands, cmd.data);
	}

	return commands;
}

/* ----- ANALYZE ----- */

static int
etcd_acquire_sample_rows(Relation relation, int elevel,
						 HeapTuple *rows, int targrows,
						 double *totalrows, double *totaldeadrows)
{
	EtcdFdwOptions *opts = etcd_get_options(RelationGetRelid(relation), GetUserId());
	EtcdConn   *conn = etcd_conn_get(opts);
	TupleDesc	td = RelationGetDescr(relation);
	int			prefix_len = (int) strlen(opts->prefix);
	EtcdRangeRequest req;
	EtcdRangeResult *r;
	int			numrows = 0;
	int			i;
	Datum	   *values = palloc(sizeof(Datum) * td->natts);
	bool	   *nulls = palloc(sizeof(bool) * td->natts);

	MemSet(&req, 0, sizeof(req));
	req.key = opts->prefix;
	req.key_len = prefix_len;
	{
		int			rel_len;
		char	   *re = etcd_prefix_range_end(opts->prefix, prefix_len, &rel_len);

		req.range_end = re;
		req.range_end_len = rel_len;
	}
	req.sort_order = ETCD_SORT_ASCEND;
	req.sort_target = ETCD_TARGET_KEY;
	r = etcd_client_range(conn, &req);

	for (i = 0; i < r->count; i++)
	{
		EtcdKV	   *kv = &r->kvs[i];
		int			a;
		int			pos;

		/* fill the tuple */
		for (a = 0; a < td->natts; a++)
		{
			Form_pg_attribute attr = TupleDescAttr(td, a);
			const char *nm;

			values[a] = (Datum) 0;
			nulls[a] = true;
			if (attr->attisdropped)
				continue;
			nm = NameStr(attr->attname);
			if (strcmp(nm, "key") == 0)
			{
				int			off = 0;
				int			len = kv->key_len;

				if (opts->strip_prefix && len >= prefix_len)
				{
					off = prefix_len;
					len -= prefix_len;
				}
				values[a] = PointerGetDatum(cstring_to_text_with_len(kv->key + off, len));
				nulls[a] = false;
			}
			else if (strcmp(nm, "value") == 0 && kv->value)
			{
				if (attr->atttypid == BYTEAOID)
				{
					bytea	   *b = (bytea *) palloc(VARHDRSZ + kv->value_len);

					SET_VARSIZE(b, VARHDRSZ + kv->value_len);
					memcpy(VARDATA(b), kv->value, kv->value_len);
					values[a] = PointerGetDatum(b);
				}
				else
					values[a] = PointerGetDatum(cstring_to_text_with_len(kv->value, kv->value_len));
				nulls[a] = false;
			}
			else if (strcmp(nm, "create_revision") == 0)
			{
				values[a] = Int64GetDatum(kv->create_revision);
				nulls[a] = false;
			}
			else if (strcmp(nm, "mod_revision") == 0)
			{
				values[a] = Int64GetDatum(kv->mod_revision);
				nulls[a] = false;
			}
			else if (strcmp(nm, "version") == 0)
			{
				values[a] = Int64GetDatum(kv->version);
				nulls[a] = false;
			}
			else if (strcmp(nm, "lease") == 0)
			{
				values[a] = Int64GetDatum(kv->lease);
				nulls[a] = false;
			}
		}

		/* reservoir sampling */
		if (numrows < targrows)
			rows[numrows++] = heap_form_tuple(td, values, nulls);
		else
		{
			pos = (int) (((double) random() / ((double) RAND_MAX + 1)) * (i + 1));
			if (pos < targrows)
			{
				heap_freetuple(rows[pos]);
				rows[pos] = heap_form_tuple(td, values, nulls);
			}
		}
	}

	*totalrows = r->count;
	*totaldeadrows = 0;
	return numrows;
}

static bool
etcdAnalyzeForeignTable(Relation relation, AcquireSampleRowsFunc *func,
						BlockNumber *totalpages)
{
	*func = etcd_acquire_sample_rows;
	*totalpages = 1;			/* refined by the sampler's totalrows */
	return true;
}

/* ----- entry points ----- */

Datum
etcd_fdw_handler(PG_FUNCTION_ARGS)
{
	FdwRoutine *routine = makeNode(FdwRoutine);

	routine->GetForeignRelSize = etcdGetForeignRelSize;
	routine->GetForeignPaths = etcdGetForeignPaths;
	routine->GetForeignPlan = etcdGetForeignPlan;
	routine->BeginForeignScan = etcdBeginForeignScan;
	routine->IterateForeignScan = etcdIterateForeignScan;
	routine->ReScanForeignScan = etcdReScanForeignScan;
	routine->EndForeignScan = etcdEndForeignScan;
	routine->ExplainForeignScan = etcdExplainForeignScan;

	/* write path */
	routine->AddForeignUpdateTargets = etcdAddForeignUpdateTargets;
	routine->PlanForeignModify = etcdPlanForeignModify;
	routine->BeginForeignModify = etcdBeginForeignModify;
	routine->ExecForeignInsert = etcdExecForeignInsert;
	routine->ExecForeignUpdate = etcdExecForeignUpdate;
	routine->ExecForeignDelete = etcdExecForeignDelete;
	routine->EndForeignModify = etcdEndForeignModify;
	routine->BeginForeignInsert = etcdBeginForeignInsert;
	routine->EndForeignInsert = etcdEndForeignInsert;
	routine->GetForeignModifyBatchSize = etcdGetForeignModifyBatchSize;
	routine->ExecForeignBatchInsert = etcdExecForeignBatchInsert;
	routine->IsForeignRelUpdatable = etcdIsForeignRelUpdatable;
	routine->ExecForeignTruncate = etcdExecForeignTruncate;

	/* schema import + analyze */
	routine->ImportForeignSchema = etcdImportForeignSchema;
	routine->AnalyzeForeignTable = etcdAnalyzeForeignTable;

	PG_RETURN_POINTER(routine);
}

Datum
etcd_fdw_validator(PG_FUNCTION_ARGS)
{
	List	   *options_list = untransformRelOptions(PG_GETARG_DATUM(0));
	Oid			catalog = PG_GETARG_OID(1);

	etcd_validate_options(options_list, catalog);

	PG_RETURN_VOID();
}

void
_PG_init(void)
{
	etcd_json_init();			/* route cJSON through palloc before any use */
	etcd_conn_init();
}

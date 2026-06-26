/*-------------------------------------------------------------------------
 *
 * deparse.c
 *	  Translate WHERE clauses on the key column into an etcd Range request.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/transam.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_type.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"

#include "deparse.h"
#include "etcd_json.h"

/* POSIX collation orders by raw bytes like C; its OID macro was removed in PG18. */
#ifndef POSIX_COLLATION_OID
#define POSIX_COLLATION_OID C_COLLATION_OID
#endif

/* byte-wise comparison matching etcd's key ordering */
static int
cmp_bytes(const char *a, int alen, const char *b, int blen)
{
	int			minlen = Min(alen, blen);
	int			c = memcmp(a, b, minlen);

	if (c != 0)
		return c < 0 ? -1 : 1;
	if (alen == blen)
		return 0;
	return alen < blen ? -1 : 1;
}

/* return a copy of k with a trailing NUL byte appended (the "next" key) */
static char *
next_key(const char *k, int klen, int *out_len)
{
	char	   *r = palloc(klen + 1);

	memcpy(r, k, klen);
	r[klen] = '\0';
	*out_len = klen + 1;
	return r;
}

/* Map a user-supplied key value to the full etcd key (apply table prefix). */
static char *
map_key(EtcdFdwOptions *opts, const char *userkey, int userlen, int *out_len)
{
	int			plen = (int) strlen(opts->prefix);

	if (opts->strip_prefix && plen > 0)
	{
		char	   *r = palloc(plen + userlen);

		memcpy(r, opts->prefix, plen);
		memcpy(r + plen, userkey, userlen);
		*out_len = plen + userlen;
		return r;
	}
	*out_len = userlen;
	return pnstrdup(userkey, userlen);
}

/* Collation gates. */
static bool collation_is_deterministic_eq(Oid coll);

static bool
collation_is_byteordered(Oid coll)
{
	return coll == C_COLLATION_OID || coll == POSIX_COLLATION_OID;
}

bool
etcd_collation_is_byteordered(Oid coll)
{
	return collation_is_byteordered(coll);
}

bool
etcd_collation_is_det_eq(Oid coll)
{
	return collation_is_deterministic_eq(coll);
}

static bool
collation_is_deterministic_eq(Oid coll)
{
	if (coll == InvalidOid || collation_is_byteordered(coll))
		return true;
	/* default collation is deterministic except for ICU nondeterministic dbs */
	if (coll == DEFAULT_COLLATION_OID)
		return get_collation_isdeterministic(coll);
	return get_collation_isdeterministic(coll);
}

/*
 * Try to interpret a clause as a pushable constraint on the key column.
 * On success, returns true and fills the bound parameters; pushed clauses are
 * exact (no recheck required).  *handled tells whether the clause was pushed.
 */
typedef struct BoundState
{
	/* inclusive lower / exclusive upper bounds (full etcd keys) */
	char	   *lo;
	int			lolen;
	char	   *hi;
	int			hilen;
	/* equality target, if any */
	bool		has_eq;
	char	   *eqkey;
	int			eqlen;
	bool		conflict;		/* provably empty */
	bool		hi_inf;			/* upper bound is end-of-keyspace (no real hi) */
	/* "key IN (...)" candidate set (full keys), if any */
	bool		in_present;
	int			nin;
	char	  **inkeys;
	int		   *inlens;
} BoundState;

/*
 * etcd uses range_end == "\0" (a single NUL byte) as a sentinel meaning "to the
 * end of the keyspace".  It must never be compared as an ordinary (tiny) key.
 */
static bool
is_keyspace_end(const char *k, int klen)
{
	return klen == 1 && k[0] == '\0';
}

static void
tighten_lo(BoundState *b, char *k, int klen)
{
	if (cmp_bytes(k, klen, b->lo, b->lolen) > 0)
	{
		b->lo = k;
		b->lolen = klen;
	}
}

static void
tighten_hi(BoundState *b, char *k, int klen)
{
	/* "to the end of the keyspace" imposes no real upper bound */
	if (is_keyspace_end(k, klen))
		return;
	if (b->hi_inf)
	{
		b->hi = k;
		b->hilen = klen;
		b->hi_inf = false;
	}
	else if (cmp_bytes(k, klen, b->hi, b->hilen) < 0)
	{
		b->hi = k;
		b->hilen = klen;
	}
}

/* is key strictly below the upper bound? (always true when hi is infinite) */
static bool
below_hi(BoundState *b, const char *k, int klen)
{
	return b->hi_inf || cmp_bytes(k, klen, b->hi, b->hilen) < 0;
}

/* Parse "prefix%" LIKE patterns into a plain prefix (no wildcards/escape). */
static bool
like_to_prefix(const char *pat, int patlen, char **prefix, int *prefixlen)
{
	int			i;

	for (i = 0; i < patlen; i++)
	{
		char		c = pat[i];

		if (c == '\\')
			return false;		/* escapes: not a simple prefix */
		if (c == '_')
			return false;
		if (c == '%')
		{
			if (i == patlen - 1)
			{
				*prefix = pnstrdup(pat, i);
				*prefixlen = i;
				return true;
			}
			return false;		/* % not at end */
		}
	}
	return false;				/* no wildcard: an exact match, handle as '=' */
}

static bool
classify_clause(EtcdRelInfo *ri, Index relid, Expr *clause, BoundState *b)
{
	OpExpr	   *op;
	char	   *opname;
	Node	   *left,
			   *right;
	Var		   *var;
	Const	   *con;
	bool		var_on_left;
	char	   *cval;
	int			clen;
	int			klen;
	char	   *fk;

	/* "key IN (array-const)" -> a multi-key fetch */
	if (IsA(clause, ScalarArrayOpExpr))
	{
		ScalarArrayOpExpr *saop = (ScalarArrayOpExpr *) clause;
		Node	   *l,
				   *r;
		Var		   *v;
		Const	   *arrc;
		char	   *opn;
		ArrayType  *arr;
		Oid			elemtype;
		int16		typlen;
		bool		typbyval;
		char		typalign;
		Datum	   *elems;
		bool	   *nulls;
		int			nelems;
		int			i;

		if (!saop->useOr || list_length(saop->args) != 2)
			return false;
		l = (Node *) linitial(saop->args);
		r = (Node *) lsecond(saop->args);
		if (IsA(l, RelabelType))
			l = (Node *) ((RelabelType *) l)->arg;
		if (!IsA(l, Var) || !IsA(r, Const))
			return false;
		v = (Var *) l;
		arrc = (Const *) r;
		if (v->varno != relid || v->varattno != ri->key_attno)
			return false;
		if (arrc->constisnull)
			return false;

		if (saop->opno >= FirstNormalObjectId)	/* built-in operators only */
			return false;
		opn = get_opname(saop->opno);
		if (opn == NULL || strcmp(opn, "=") != 0)
			return false;
		if (!collation_is_deterministic_eq(ri->key_collation))
			return false;
		if (b->in_present)		/* only one IN clause is pushed */
			return false;

		arr = DatumGetArrayTypeP(arrc->constvalue);
		elemtype = ARR_ELEMTYPE(arr);
		if (!(elemtype == TEXTOID || elemtype == VARCHAROID))
			return false;
		get_typlenbyvalalign(elemtype, &typlen, &typbyval, &typalign);
		deconstruct_array(arr, elemtype, typlen, typbyval, typalign,
						  &elems, &nulls, &nelems);

		b->inkeys = (char **) palloc(sizeof(char *) * Max(nelems, 1));
		b->inlens = (int *) palloc(sizeof(int) * Max(nelems, 1));
		b->nin = 0;
		for (i = 0; i < nelems; i++)
		{
			char	   *ev;
			int			evlen;

			if (nulls[i])
				continue;
			ev = TextDatumGetCString(elems[i]);
			b->inkeys[b->nin] = map_key(ri->opts, ev, (int) strlen(ev), &evlen);
			b->inlens[b->nin] = evlen;
			b->nin++;
		}
		b->in_present = true;
		return true;
	}

	if (!IsA(clause, OpExpr))
		return false;
	op = (OpExpr *) clause;
	if (list_length(op->args) != 2)
		return false;

	left = (Node *) linitial(op->args);
	right = (Node *) lsecond(op->args);

	/* unwrap relabel/coerce */
	if (IsA(left, RelabelType))
		left = (Node *) ((RelabelType *) left)->arg;
	if (IsA(right, RelabelType))
		right = (Node *) ((RelabelType *) right)->arg;

	if (IsA(left, Var) && IsA(right, Const))
	{
		var = (Var *) left;
		con = (Const *) right;
		var_on_left = true;
	}
	else if (IsA(right, Var) && IsA(left, Const))
	{
		var = (Var *) right;
		con = (Const *) left;
		var_on_left = false;
	}
	else
		return false;

	/* must be our key column */
	if (var->varno != relid || var->varattno != ri->key_attno)
		return false;
	if (con->constisnull)
		return false;
	/*
	 * Only text/varchar constants are byte-comparable to the key the way etcd
	 * compares.  (bpchar carries blank padding; unknown is a cstring, not a
	 * varlena, so reading it as text would be unsafe.)
	 */
	if (!(con->consttype == TEXTOID || con->consttype == VARCHAROID))
		return false;

	/*
	 * Only push genuine built-in comparison operators.  Matching by name alone
	 * could mis-push a user-defined operator that happens to be named "=", "<",
	 * etc. with different semantics; built-in operators have OIDs below
	 * FirstNormalObjectId.
	 */
	if (op->opno >= FirstNormalObjectId)
		return false;

	opname = get_opname(op->opno);
	if (opname == NULL)
		return false;

	cval = TextDatumGetCString(con->constvalue);
	clen = (int) strlen(cval);

	/* ^@ : starts-with (needs byte ordering) */
	if (strcmp(opname, "^@") == 0 && var_on_left)
	{
		char	   *pfx;
		int			plen;
		char	   *end;
		int			endlen;

		if (!collation_is_byteordered(ri->key_collation))
			return false;
		pfx = map_key(ri->opts, cval, clen, &plen);
		tighten_lo(b, pfx, plen);
		end = etcd_prefix_range_end(pfx, plen, &endlen);
		tighten_hi(b, end, endlen);
		return true;
	}

	/* LIKE 'prefix%' */
	if (strcmp(opname, "~~") == 0 && var_on_left)
	{
		char	   *upat;
		int			ulen;
		char	   *fkk;
		char	   *end;
		int			endlen;

		if (!collation_is_byteordered(ri->key_collation))
			return false;
		if (!like_to_prefix(cval, clen, &upat, &ulen))
			return false;
		fkk = map_key(ri->opts, upat, ulen, &klen);
		tighten_lo(b, fkk, klen);
		end = etcd_prefix_range_end(fkk, klen, &endlen);
		tighten_hi(b, end, endlen);
		return true;
	}

	/* equality (deterministic collations only) */
	if (strcmp(opname, "=") == 0)
	{
		if (!collation_is_deterministic_eq(ri->key_collation))
			return false;
		fk = map_key(ri->opts, cval, clen, &klen);
		if (b->has_eq)
		{
			if (cmp_bytes(fk, klen, b->eqkey, b->eqlen) != 0)
				b->conflict = true;
		}
		else
		{
			b->has_eq = true;
			b->eqkey = fk;
			b->eqlen = klen;
		}
		return true;
	}

	/* range comparisons require byte ordering */
	if (!collation_is_byteordered(ri->key_collation))
		return false;

	/* normalise operator if the Var is on the right */
	if (!var_on_left)
	{
		if (strcmp(opname, "<") == 0)
			opname = ">";
		else if (strcmp(opname, "<=") == 0)
			opname = ">=";
		else if (strcmp(opname, ">") == 0)
			opname = "<";
		else if (strcmp(opname, ">=") == 0)
			opname = "<=";
		else
			return false;
	}

	fk = map_key(ri->opts, cval, clen, &klen);

	if (strcmp(opname, ">=") == 0)
		tighten_lo(b, fk, klen);
	else if (strcmp(opname, ">") == 0)
	{
		int			nlen;
		char	   *nk = next_key(fk, klen, &nlen);

		tighten_lo(b, nk, nlen);
	}
	else if (strcmp(opname, "<") == 0)
		tighten_hi(b, fk, klen);
	else if (strcmp(opname, "<=") == 0)
	{
		int			nlen;
		char	   *nk = next_key(fk, klen, &nlen);

		tighten_hi(b, nk, nlen);
	}
	else
		return false;

	return true;
}

void
etcd_deparse_analyze(PlannerInfo *root, RelOptInfo *baserel,
					 Oid foreigntableid, EtcdRelInfo *ri)
{
	Relation	rel;
	TupleDesc	td;
	int			i;
	BoundState	b;
	EtcdScanSpec *spec = palloc0(sizeof(EtcdScanSpec));
	int			plen = (int) strlen(ri->opts->prefix);
	char	   *base_end;
	int			base_end_len;
	ListCell   *lc;

	/* locate the key column and its collation/type */
	rel = table_open(foreigntableid, NoLock);
	td = RelationGetDescr(rel);
	ri->key_attno = 0;
	ri->key_collation = InvalidOid;
	for (i = 0; i < td->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(td, i);

		if (!attr->attisdropped && strcmp(NameStr(attr->attname), "key") == 0)
		{
			ri->key_attno = attr->attnum;
			ri->key_collation = attr->attcollation;
			break;
		}
	}
	table_close(rel, NoLock);

	ri->pushdown_safe_order = collation_is_byteordered(ri->key_collation);

	/* base range = [prefix, prefix_range_end).  An empty (or all-0xFF) prefix
	 * yields the "end of keyspace" sentinel, i.e. no real upper bound. */
	base_end = etcd_prefix_range_end(ri->opts->prefix, plen, &base_end_len);
	MemSet(&b, 0, sizeof(b));
	b.lo = pnstrdup(ri->opts->prefix, plen);
	b.lolen = plen;
	b.hi = base_end;
	b.hilen = base_end_len;
	b.hi_inf = is_keyspace_end(base_end, base_end_len);

	ri->remote_conds = NIL;
	ri->local_conds = NIL;

	/* classify each base restriction */
	foreach(lc, baserel->baserestrictinfo)
	{
		RestrictInfo *rinfo = lfirst_node(RestrictInfo, lc);

		if (ri->key_attno != 0 &&
			classify_clause(ri, baserel->relid, rinfo->clause, &b))
			ri->remote_conds = lappend(ri->remote_conds, rinfo);
		else
			ri->local_conds = lappend(ri->local_conds, rinfo);
	}

	/* resolve the bounds into a concrete scan spec */
	if (b.conflict)
	{
		spec->empty = true;
	}
	else if (b.in_present)
	{
		/*
		 * Multi-key fetch.  Keep only IN values that fall within the range
		 * bounds and (if present) match the equality target, then dedup.
		 */
		int			ki,
					kj;
		char	  **keys = palloc(sizeof(char *) * Max(b.nin, 1));
		int		   *lens = palloc(sizeof(int) * Max(b.nin, 1));
		int			n = 0;

		for (ki = 0; ki < b.nin; ki++)
		{
			char	   *k = b.inkeys[ki];
			int			kl = b.inlens[ki];
			bool		dup = false;

			if (cmp_bytes(k, kl, b.lo, b.lolen) < 0 || !below_hi(&b, k, kl))
				continue;
			if (b.has_eq && cmp_bytes(k, kl, b.eqkey, b.eqlen) != 0)
				continue;
			for (kj = 0; kj < n; kj++)
				if (cmp_bytes(k, kl, keys[kj], lens[kj]) == 0)
				{
					dup = true;
					break;
				}
			if (dup)
				continue;
			keys[n] = k;
			lens[n] = kl;
			n++;
		}

		if (n == 0)
			spec->empty = true;
		else
		{
			spec->nkeys = n;
			spec->keys = keys;
			spec->keylens = lens;
		}
	}
	else if (b.has_eq)
	{
		/* single key, but only if it falls inside the remaining range */
		if (cmp_bytes(b.eqkey, b.eqlen, b.lo, b.lolen) >= 0 &&
			below_hi(&b, b.eqkey, b.eqlen))
		{
			spec->single_key = true;
			spec->start = b.eqkey;
			spec->start_len = b.eqlen;
		}
		else
			spec->empty = true;
	}
	else
	{
		/* empty only when a real (finite) hi is at or below lo */
		if (!b.hi_inf && cmp_bytes(b.lo, b.lolen, b.hi, b.hilen) >= 0)
			spec->empty = true;
		else
		{
			spec->start = b.lo;
			spec->start_len = b.lolen;
			if (b.hi_inf)
			{
				/* no upper bound: scan to the end of the keyspace */
				spec->end = base_end;	/* the "\0" sentinel */
				spec->end_len = base_end_len;
			}
			else
			{
				spec->end = b.hi;
				spec->end_len = b.hilen;
			}
		}
	}

	spec->sort_order = ETCD_SORT_ASCEND;
	spec->limit = 0;

	ri->spec = spec;
}

/* ----- serialisation for the executor ----- */

List *
etcd_serialize_spec(EtcdScanSpec *spec)
{
	List	   *l = NIL;
	List	   *keys = NIL;
	int			i;

	l = lappend(l, makeString(spec->start ?
							  etcd_b64_encode(spec->start, spec->start_len) :
							  pstrdup("")));
	l = lappend(l, makeString(spec->end ?
							  etcd_b64_encode(spec->end, spec->end_len) :
							  pstrdup("")));
	l = lappend(l, makeInteger(spec->single_key ? 1 : 0));
	l = lappend(l, makeInteger(spec->empty ? 1 : 0));
	l = lappend(l, makeInteger((int) spec->limit));
	l = lappend(l, makeInteger((int) spec->sort_order));
	l = lappend(l, makeInteger(spec->param_key ? 1 : 0));

	for (i = 0; i < spec->nkeys; i++)
		keys = lappend(keys, makeString(etcd_b64_encode(spec->keys[i],
														spec->keylens[i])));
	l = lappend(l, keys);		/* nested list (may be NIL) */
	return l;
}

EtcdScanSpec *
etcd_deserialize_spec(List *list)
{
	EtcdScanSpec *spec = palloc0(sizeof(EtcdScanSpec));
	char	   *s0 = strVal(list_nth(list, 0));
	char	   *s1 = strVal(list_nth(list, 1));
	List	   *keys;
	ListCell   *lc;
	int			i;

	spec->single_key = (intVal(list_nth(list, 2)) != 0);
	spec->empty = (intVal(list_nth(list, 3)) != 0);
	spec->limit = intVal(list_nth(list, 4));
	spec->sort_order = (EtcdSortOrder) intVal(list_nth(list, 5));
	spec->param_key = (intVal(list_nth(list, 6)) != 0);

	spec->start = etcd_b64_decode(s0, &spec->start_len);
	if (spec->single_key)
		spec->end = NULL;
	else
		spec->end = etcd_b64_decode(s1, &spec->end_len);

	keys = (List *) list_nth(list, 7);
	spec->nkeys = list_length(keys);
	if (spec->nkeys > 0)
	{
		spec->keys = palloc(sizeof(char *) * spec->nkeys);
		spec->keylens = palloc(sizeof(int) * spec->nkeys);
		i = 0;
		foreach(lc, keys)
		{
			int			len;

			spec->keys[i] = etcd_b64_decode(strVal(lfirst(lc)), &len);
			spec->keylens[i] = len;
			i++;
		}
	}

	return spec;
}

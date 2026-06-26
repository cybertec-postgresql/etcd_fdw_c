# Predicate pushdown

`etcd_fdw` translates restrictions on the `key` column into a single etcd
`Range` request. Anything it cannot represent exactly is left for PostgreSQL to
recheck locally, so results are always correct.

## What is pushed down

| SQL | etcd Range | Requires C/POSIX collation on `key` |
|---|---|---|
| `key = 'x'` | single-key get | no (any deterministic collation) |
| `key >= 'a'`, `key > 'a'` | range start | yes |
| `key < 'b'`, `key <= 'b'` | range end | yes |
| `a <= key AND key < b` | bounded range | yes |
| `key LIKE 'p%'` (plain prefix) | prefix range `[p, p⁺)` | yes |
| `key ^@ 'p'` (starts-with) | prefix range | yes |
| `key IN ('a','b',...)` | one get per key (multi-key fetch) | no (any deterministic collation) |
| `ORDER BY key [ASC\|DESC]` | `sort_target=KEY`, `sort_order` | yes |
| `LIMIT n` (no local filter) | `limit` | n/a |

Multiple key predicates combine into one tightened range. A contradictory set
(e.g. `key = 'a' AND key = 'b'`) is recognised as empty and skips the request.
An `IN` list becomes a multi-key fetch (`EXPLAIN` shows `etcd Scan keys: N`); its
values are filtered to those inside the table's prefix/range and deduplicated.

## Parameterized joins

A join on the key column, `... JOIN cfg ON cfg.key = other.col`, can be executed
as a nested loop where each outer row drives a **single etcd key lookup** instead
of scanning the whole prefix. `etcd_fdw` offers a parameterized path for this;
`EXPLAIN` shows `etcd Scan: parameterized key` when the planner chooses it
(typically when the foreign side is large relative to the outer side — set
`use_remote_estimate 'true'` so the planner sees the real cardinality). The key
equality must use a deterministic collation. When a plain (non-parameterized)
plan is cheaper, the planner uses that instead — both are correct.

Filters on non-key columns (e.g. `value = '...'`) are **not** pushed down; they
appear as a `Filter` in the plan and are applied by PostgreSQL.

## Why collation matters

etcd compares keys byte-by-byte. PostgreSQL `<`/`>`/`ORDER BY` on `text` use the
column's collation, which for locales like `en_US` does **not** match byte order.
Pushing such comparisons down would yield wrong results, so `etcd_fdw` only
pushes range/prefix/ORDER BY when the `key` column collation is `C` or `POSIX`.
Equality is byte-exact for any deterministic collation and is always pushed.

Declare the key column as `key text COLLATE "C"` to get full pushdown.

## Inspecting plans

```sql
EXPLAIN (VERBOSE, COSTS OFF) SELECT key FROM cfg WHERE key >= 'a' AND key < 'm' ORDER BY key;
```
```
 Foreign Scan on cfg
   Output: key
   etcd Endpoint: http://127.0.0.1:2379
   etcd Prefix: /app/config/
   etcd Scan: range          -- single key | range | (provably empty)
   etcd Order: ASC           -- or DESC
```

`etcd Limit: N` appears when a `LIMIT` was pushed down, and a `Filter:` line
appears for any locally-rechecked qualifier.

## Cost estimation

By default the planner uses a static row estimate. Set `use_remote_estimate
'true'` on the server or table to issue a cheap `count_only` Range at plan time
for accurate cardinality, at the cost of one extra round trip per planned scan.

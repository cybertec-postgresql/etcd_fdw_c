# Transactions & concurrency

## etcd is not transactional with PostgreSQL

etcd has no multi-statement transactions that span requests, and it does not
participate in PostgreSQL's transaction or two-phase commit. **Writes made
through `etcd_fdw` are not rolled back when the surrounding PostgreSQL
transaction aborts.**

Concretely:

```sql
BEGIN;
INSERT INTO cfg (key, value) VALUES ('a', '1');   -- written to etcd immediately
ROLLBACK;                                          -- the etcd key 'a' REMAINS
```

Treat each row modification as an independent operation against etcd. Do not
rely on PostgreSQL transaction boundaries for atomicity or rollback of etcd
data. This is an inherent property of the data source, not a limitation that
will be removed.

## Per-statement atomicity

A single `UPDATE` or `DELETE` of one row is executed as one etcd transaction
(`/v3/kv/txn`): the comparison and the write happen atomically inside etcd. A
key rename (`UPDATE ... SET key = ...`) performs the new `Put` and the old
`Delete` in the same etcd transaction.

## Optimistic concurrency

When the foreign table includes a `mod_revision` column, `etcd_fdw` reads each
row's `mod_revision` during the scan and uses it as a guard: the modifying etcd
transaction only proceeds if the key's `mod_revision` is unchanged. If another
client modified the key in between, the transaction fails its comparison and
`etcd_fdw` raises:

```
ERROR:  etcd_fdw: row was modified concurrently in etcd
```

(SQLSTATE `40001`, serialization failure) — retry the statement.

If the table has **no** `mod_revision` column, modifications are applied
unconditionally (last writer wins) with no concurrency check.

## INSERT semantics

`INSERT` maps to an etcd `Put`, which is an **upsert**: inserting a key that
already exists overwrites its value rather than raising a duplicate-key error.

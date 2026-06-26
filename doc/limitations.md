# Limitations

- **Not transactional with PostgreSQL.** etcd writes are applied immediately and
  are not rolled back on PostgreSQL `ROLLBACK`. See
  [transactions.md](transactions.md).
- **INSERT is an upsert.** Inserting an existing key overwrites it (etcd `Put`
  semantics); no duplicate-key error.
- **Bulk inserts are coalesced into batches of ~100 rows**, each applied as one
  etcd transaction to stay under etcd's default `--max-txn-ops` (128). A batch
  is atomic; a multi-batch statement is not atomic across batches.
- **Range/prefix/ORDER BY pushdown requires `COLLATE "C"`** on the `key` column,
  because etcd orders by raw bytes. With other collations these are evaluated
  locally (still correct, just less efficient). See [pushdown.md](pushdown.md).
- **Keys with embedded NUL bytes** are not supported through `text` columns.
- **No aggregate pushdown.** Aggregates run in PostgreSQL after rows are fetched.
  Equality joins on the key column *can* be pushed as parameterized single-key
  lookups (see [pushdown.md](pushdown.md)); other join conditions are local.
- **No `Watch` / change-data-capture.** The wrapper performs point-in-time
  Range/Put/Delete/Txn operations; it does not stream etcd watches.
- **`IMPORT FOREIGN SCHEMA`** creates one table per immediate child "directory"
  (keys split on `/`) under the imported prefix, with the standard column set.
  Leaf keys directly under the prefix are not given their own table.
- **`ANALYZE`** samples by scanning the table's prefix; for very large prefixes
  this reads all keys to sample.
- **Leases** are managed through dedicated functions —
  `etcd_fdw_lease_grant()`, `etcd_fdw_lease_ttl()`, `etcd_fdw_lease_keepalive()`
  and `etcd_fdw_lease_revoke()` — and an existing lease id can be attached to a
  row via the `lease` column on `INSERT`/`UPDATE`. There is no automatic
  background keepalive: a lease is refreshed only when you call
  `etcd_fdw_lease_keepalive()`. See [reference.md](reference.md).
- **No server-side cursors / streaming** of large result sets: a scan buffers
  results in backend memory (following etcd's `more` flag for large ascending
  scans).

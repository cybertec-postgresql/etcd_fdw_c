# Changelog

## 1.3

First public release.

etcd_fdw is a PostgreSQL foreign-data wrapper for etcd v3: it maps an etcd
key/value store to foreign tables you can read and write with plain SQL.

- Reads with key-predicate pushdown -- equality, range, prefix (`LIKE 'p%'`,
  `^@`), `IN`, `ORDER BY key` and `LIMIT` are translated to etcd `Range`
  (range/prefix/order require a `C`/`POSIX` key collation; anything else is
  rechecked locally).
- Parameterized joins: `key = outer.col` runs as one etcd key lookup per outer
  row.
- Writes: `INSERT` (with batching), `UPDATE` (including key rename), `DELETE`,
  `COPY` and `TRUNCATE`, with optimistic concurrency guarded on `mod_revision`.
- `IMPORT FOREIGN SCHEMA` and `ANALYZE`.
- Lease management from SQL: grant, ttl, keepalive, revoke.
- TLS and etcd RBAC authentication, with certificate-expiry monitoring
  functions and a view.
- High availability: multiple endpoints with automatic failover and bounded
  retries.

Supports PostgreSQL 14-18 and etcd 3.4 / 3.5 / 3.6.

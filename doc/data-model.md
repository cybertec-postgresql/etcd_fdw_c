# Data model

etcd is a flat, ordered key/value store. `etcd_fdw` maps a **key prefix** to a
foreign table; each key under that prefix is one row.

## Rows and columns

A row corresponds to one etcd key/value pair. The available columns (matched by
name) are:

```
key text, value text|bytea, create_revision bigint,
mod_revision bigint, version bigint, lease bigint
```

You only declare the columns you care about. For example a config table might be
just `(key text, value text)`.

## Prefixes and `strip_prefix`

A table maps to `server.prefix || table.prefix`. With `strip_prefix 'true'`, the
`key` column shows the part **after** the prefix, and writes prepend it back:

```sql
CREATE FOREIGN TABLE cfg (key text COLLATE "C", value text)
  SERVER etcd OPTIONS (prefix '/app/config/', strip_prefix 'true');
-- etcd key /app/config/host  <-->  key = 'host'
```

With `strip_prefix 'false'` (default) the `key` column is the full etcd key, and
the table is still scoped to the prefix range.

An empty prefix maps the whole keyspace.

## Key ordering and collation

etcd orders keys by **raw byte value**. PostgreSQL text ordering depends on the
column collation. To make pushdown of range, prefix and `ORDER BY` correct,
declare the `key` column with `COLLATE "C"` (or `"POSIX"`), which matches etcd's
byte ordering. Equality pushdown works for any deterministic collation.

If the `key` column uses a non-C collation, equality is still pushed down, but
range/prefix/ORDER BY are evaluated locally by PostgreSQL.

## Values and base64

etcd's JSON gateway transports keys and values as base64; `etcd_fdw` encodes and
decodes transparently. Use a `bytea` `value` column for arbitrary binary data;
`text` is appropriate for UTF-8/string values.

> Keys containing NUL bytes are not representable through `text` columns and are
> not supported.

## Revisions, versions and leases

- `create_revision` / `mod_revision` are **cluster-global**, monotonically
  increasing counters — their absolute values are not meaningful across keys
  except for ordering and concurrency control.
- `version` is **per key** and resets to 1 when a key is deleted and recreated.
- `lease` is the attached lease id; write a non-zero value to bind a key to a
  lease (the lease must already exist in etcd). Manage leases from SQL with
  `etcd_fdw_lease_grant()` / `etcd_fdw_lease_ttl()` / `etcd_fdw_lease_keepalive()`
  / `etcd_fdw_lease_revoke()` — see [reference.md](reference.md) and
  [examples.md](examples.md).

# Troubleshooting

### `ERROR: etcd_fdw: could not reach any etcd endpoint`
None of the configured `endpoints` accepted a connection. Check the URLs and
scheme (`http://` vs `https://`), network reachability from the PostgreSQL host,
and `connect_timeout_ms`. `DETAIL` carries the last libcurl transport error.

### `ERROR: ... range failed (HTTP 400) ... user name is empty`
etcd has authentication enabled but no credentials were sent. Create a user
mapping with `username`/`password`. If you added the mapping mid-session it now
takes effect immediately (the connection cache is invalidated on user-mapping
changes).

### `ERROR: etcd_fdw: row was modified concurrently in etcd` (SQLSTATE 40001)
Optimistic-concurrency conflict: the key's `mod_revision` changed between the
scan and the write. Retry the statement. To opt out of the check, omit the
`mod_revision` column from the table (last-writer-wins).

### Range / `ORDER BY` predicates are not pushed down
The `key` column must use `COLLATE "C"` (or `"POSIX"`) for range, prefix and
`ORDER BY` pushdown. Check the plan with `EXPLAIN (VERBOSE)` — a `Filter:` line
or a `Sort` node above the scan means the predicate/ordering was applied locally.

### TLS handshake failures
Ensure `endpoints` use `https://`, `use_tls 'true'` is set, and `cafile` points
to the CA that signed the etcd server certificate. The certificate SAN must match
the endpoint hostname. For mutual TLS also set `certfile`/`keyfile`. As a last
resort for testing only, `tls_verify 'false'` disables verification.

### Wrong rows after changing collation
If you `ALTER` the `key` column collation, re-check plans: pushdown eligibility
depends on the collation being `C`/`POSIX`.

### Inspecting what the wrapper sends
`EXPLAIN (VERBOSE, COSTS OFF)` shows the endpoint, prefix, scan type
(single key / range / empty), pushed `LIMIT`, and order. Compare against
`etcdctl get --prefix <prefix>` to confirm the data.

# Option reference

All options are passed via the standard `OPTIONS (...)` clause and validated by
`etcd_fdw_validator` at DDL time.

## Server options (`CREATE SERVER`)

| Option | Type | Default | Description |
|---|---|---|---|
| `endpoints` | text | `http://127.0.0.1:2379` | Comma-separated list of etcd client URLs (`host:port` or full URL). The first reachable endpoint is used; the rest provide failover. A bare `host:port` is assumed to be `http://`. |
| `use_tls` | bool | `false` | Connect over HTTPS. Use `https://` URLs in `endpoints`. |
| `cafile` | text | – | Path to a CA certificate bundle (PEM) used to verify the etcd server. |
| `certfile` | text | – | Client certificate (PEM) for mutual TLS. |
| `keyfile` | text | – | Client private key (PEM) for mutual TLS. |
| `tls_verify` | bool | `true` | Verify the server certificate and hostname. Set `false` only for testing. |
| `connect_timeout_ms` | int | `3000` | Connection timeout in milliseconds. |
| `request_timeout_ms` | int | `30000` | Per-request timeout in milliseconds. |
| `max_retries` | int | `2` | Extra full passes over the endpoint list before giving up (so up to `max_retries + 1` attempts). Covers transient unavailability such as a leader election. `0` disables retries. |
| `retry_backoff_ms` | int | `200` | Base backoff between retry passes (multiplied by the pass number). `0` retries with no delay. |
| `prefix` | text | – | A key prefix prepended to every table's prefix on this server. |
| `use_remote_estimate` | bool | `false` | Issue a `count_only` Range at plan time for accurate row estimates (one extra round trip per planned scan). |

The same connection options (`endpoints`, `use_tls`, `tls_verify`,
`connect_timeout_ms`, `request_timeout_ms`, `max_retries`, `retry_backoff_ms`)
may also be set on the `CREATE FOREIGN DATA WRAPPER` to provide defaults for
every server that uses the wrapper; a value set on the server overrides it.

## User mapping options (`CREATE USER MAPPING`)

| Option | Type | Description |
|---|---|---|
| `username` | text | etcd RBAC user. Triggers token authentication via `/v3/auth/authenticate`. |
| `password` | text | Password for the etcd user. |

A user mapping is optional; with none, no authentication is attempted.

## Foreign table options (`CREATE FOREIGN TABLE`)

| Option | Type | Default | Description |
|---|---|---|---|
| `prefix` | text | `''` | The etcd key prefix this table represents. Combined with the server `prefix`. |
| `strip_prefix` | bool | `false` | Present the `key` column with the table prefix removed (and prepend it again on writes). |
| `use_remote_estimate` | bool | server value | Per-table override of remote row estimation. |

## Columns

Columns are matched **by name**; define the subset you need. Unknown column
names are always NULL on read.

| Column name | Type | Meaning |
|---|---|---|
| `key` | `text` | The etcd key (stripped of the prefix when `strip_prefix` is on). Use `COLLATE "C"` to enable range/prefix/ORDER BY pushdown. Required for an updatable table. |
| `value` | `text` or `bytea` | The value. Use `bytea` for binary values. |
| `create_revision` | `bigint` | Revision at which the key was created. |
| `mod_revision` | `bigint` | Revision of the last modification (used as the optimistic-concurrency guard for UPDATE/DELETE). |
| `version` | `bigint` | Number of modifications since creation (resets to 1 on re-create). |
| `lease` | `bigint` | Attached lease id (`0` = none). Writable to attach a lease on INSERT/UPDATE; create one with `etcd_fdw_lease_grant()` (see lease functions below). |

## Functions and views (TLS certificate monitoring)

Available from extension version 1.1. Callable by a superuser, a `pg_monitor`
member, or a role with `USAGE` on the foreign server (see
[security.md](security.md)).

| Object | Signature → result | Description |
|---|---|---|
| `etcd_fdw_certificates` | `(server text)` → `TABLE(source text, subject text, issuer text, not_before timestamptz, not_after timestamptz, expires_in interval)` | One row per certificate for the server: the peer cert etcd presents (`source` = `peer`/`peer-chain`) and the local `cafile`/`certfile`. Returns no rows for a non-TLS server. |
| `etcd_fdw_server_cert_expiry` | `(server text)` → `timestamptz` | The etcd server (leaf) certificate's `not_after`; `NULL` if the server isn't TLS or no cert could be read. |
| `etcd_fdw_cert_expiry` | view → `(server, source, subject, issuer, not_before, not_after, expires_in)` | The above across every `etcd_fdw` server; rows for servers the caller can't inspect are omitted. |

## Operational functions

| Object | Signature → result | Description |
|---|---|---|
| `etcd_fdw_disconnect` | `()` → `integer` | Drop this backend's cached etcd connections (returns how many). The next query reconnects, re-reading TLS certificate files and re-authenticating. Useful after an in-place certificate rotation. Available from extension version 1.2. |

## Lease management functions

Available from extension version 1.3. Each takes the foreign **server name** and
operates on that server's etcd cluster. Callable by a superuser or a role with
`USAGE` on the server (the same privilege needed to read/write its tables).

| Object | Signature → result | Description |
|---|---|---|
| `etcd_fdw_lease_grant` | `(server text, ttl bigint)` → `bigint` | Grant a lease that lives `ttl` seconds; returns its lease id (decimal). Use that id in a row's `lease` column so the key expires with the lease. |
| `etcd_fdw_lease_ttl` | `(server text, lease bigint)` → `bigint` | Remaining TTL in seconds; `-1` if the lease does not exist or has expired. |
| `etcd_fdw_lease_keepalive` | `(server text, lease bigint)` → `bigint` | Refresh the lease once; returns the new remaining TTL. |
| `etcd_fdw_lease_revoke` | `(server text, lease bigint)` → `boolean` | Revoke the lease, which **deletes every key attached to it**. Returns `true` on success. |

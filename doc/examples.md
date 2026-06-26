# Examples & cookbook

This is a hands-on tour of `etcd_fdw`, from first connection to production
patterns. Every example was run against a live PostgreSQL + etcd pair, and the
output shown is real (trimmed for width).

If you want to follow along, you need an etcd reachable at
`http://127.0.0.1:2379` and a database where the extension is installed. The
commands build on each other within a section but each section is otherwise
self-contained.

Two conventions are used throughout:

- `etcdctl` runs etcd-side commands. etcd v3 is assumed (the default since
  etcd 3.4); if your `etcdctl` is older, prefix commands with `ETCDCTL_API=3`.
- The `key` column is declared `COLLATE "C"`. etcd orders keys by raw byte
  value, and the `"C"` collation makes PostgreSQL agree, which is what lets
  range, prefix and `ORDER BY` predicates push down to etcd. With any other
  collation those still work, but PostgreSQL evaluates them locally after
  fetching the prefix. See [pushdown.md](pushdown.md) for the full rules.

---

## 1. Setup

Before you can read anything you need three objects: the extension, a *server*
that points at your etcd cluster, and one or more *foreign tables* that map a
key prefix to columns.

### Example 1 — Load the extension

This registers the foreign-data wrapper, its handler/validator functions, and
(from version 1.1) the certificate-monitoring functions.

```sql
CREATE EXTENSION etcd_fdw;
```

### Example 2 — Define a server (plaintext)

A server records how to reach etcd. The only required option is `endpoints`.

```sql
CREATE SERVER etcd
  FOREIGN DATA WRAPPER etcd_fdw
  OPTIONS (endpoints 'http://127.0.0.1:2379');
```

### Example 3 — Define a server with multiple endpoints (failover)

List every client URL of your cluster. The wrapper fails over between them and
retries briefly, so a single node failing or a leader election does not surface
as an error. See [high-availability.md](high-availability.md).

```sql
CREATE SERVER etcd_cluster
  FOREIGN DATA WRAPPER etcd_fdw
  OPTIONS (endpoints 'http://n1:2379,http://n2:2379,http://n3:2379',
           connect_timeout_ms '2000', request_timeout_ms '10000');
```

### Example 4 — Define a TLS server

Point at `https://` endpoints, turn on `use_tls`, and give the CA that signed
the etcd server certificate. The certificate's SAN must match the host in
`endpoints`.

```sql
CREATE SERVER etcd_tls
  FOREIGN DATA WRAPPER etcd_fdw
  OPTIONS (endpoints 'https://etcd.internal:2379',
           use_tls 'true',
           cafile  '/etc/pki/etcd/ca.crt');
```

### Example 5 — User mapping for etcd RBAC authentication

When etcd has authentication enabled, supply credentials through a user mapping.
The wrapper authenticates lazily, caches the token, and re-authenticates on a
401. Map different PostgreSQL roles to different etcd users to scope access.

```sql
CREATE USER MAPPING FOR CURRENT_USER SERVER etcd
  OPTIONS (username 'app', password 's3cret');
```

### Example 6 — Prefix-scoped foreign table (stripped keys)

This is the common shape: one table maps to one key prefix. With
`strip_prefix 'true'` the `key` column shows the part *after* the prefix, and
writes prepend it again, so you work with short, readable keys.

```sql
CREATE FOREIGN TABLE app_config (
  key             text COLLATE "C",
  value           text,
  create_revision bigint,
  mod_revision    bigint,
  version         bigint,
  lease           bigint
) SERVER etcd
  OPTIONS (prefix '/app/config/', strip_prefix 'true');
-- etcd key /app/config/host  <->  key = 'host'
```

You only have to declare the columns you care about. A config table is often
just `(key text, value text)`; the revision/lease columns are there when you
need them (see [data-model.md](data-model.md)).

### Example 7 — Foreign table exposing full keys

Without `strip_prefix`, the `key` column is the full etcd key. The table is
still scoped to the prefix, so a scan only sees keys underneath it.

```sql
CREATE FOREIGN TABLE app_config_full (key text COLLATE "C", value text)
  SERVER etcd OPTIONS (prefix '/app/config/');
-- key = '/app/config/host'
```

### Example 8 — Binary values with bytea

Declare `value` as `bytea` to store and retrieve arbitrary binary data; the
wrapper handles base64 transport transparently.

```sql
CREATE FOREIGN TABLE blobs (key text COLLATE "C", value bytea)
  SERVER etcd OPTIONS (prefix '/blobs/', strip_prefix 'true');
```

### Example 9 — Whole-keyspace table (empty prefix)

An empty prefix maps the entire keyspace. This is handy for ad-hoc inspection;
scope your queries with `WHERE key LIKE '...'` so you do not pull everything.

```sql
CREATE FOREIGN TABLE etcd_all (key text COLLATE "C", value text)
  SERVER etcd OPTIONS (prefix '');
```

---

## 2. Putting data into etcd

You can populate etcd however you like — `etcd_fdw` reads whatever is there.
These examples seed the `/app/config/` prefix used by the read examples below.

### Example 10 — Seed keys with etcdctl

```bash
etcdctl put /app/config/host db.example.com
etcdctl put /app/config/port 5432
etcdctl put /app/config/name etcd_fdw
```

### Example 11 — Read it back with etcdctl

`etcdctl` prints alternating key and value lines, which is useful for confirming
what the FDW will see.

```bash
etcdctl get --prefix /app/config/
# /app/config/host
# db.example.com
# /app/config/name
# etcd_fdw
# /app/config/port
# 5432
```

### Example 12 — Write via the etcd v3 HTTP/JSON gateway (curl)

The FDW talks to etcd's JSON gateway, and you can too. Keys and values are
base64 in this API, so encode them. This is the same gateway the wrapper uses
internally, so anything you can do here, the wrapper can do.

```bash
# key=/app/config/host value=db.example.com
curl -s http://127.0.0.1:2379/v3/kv/put -X POST -d "{
  \"key\":   \"$(printf '/app/config/host' | base64)\",
  \"value\": \"$(printf 'db.example.com'   | base64)\"
}"
```

### Example 13 — Range read via the gateway (curl)

To read a prefix, send the prefix as `key` and the prefix with its last byte
incremented as `range_end`. Values come back base64-encoded.

```bash
curl -s http://127.0.0.1:2379/v3/kv/range -X POST -d "{
  \"key\":       \"$(printf '/app/config/' | base64)\",
  \"range_end\": \"$(printf '/app/config0' | base64)\"
}"
# {"header":{...},"kvs":[{"key":"L2FwcC9jb25maWcvaG9zdA==",
#  "value":"ZGIuZXhhbXBsZS5jb20=", ...}], ...}
```

### Example 14 — Seed through the FDW instead of etcdctl

Because the FDW is read-write, you can also just `INSERT`. This is the easiest
way to get going from inside PostgreSQL.

```sql
INSERT INTO app_config (key, value) VALUES
  ('host', 'db.example.com'),
  ('port', '5432'),
  ('name', 'etcd_fdw');
```

---

## 3. Reading

With `/app/config/` populated, here is how different query shapes behave. Watch
for which ones push their work into etcd versus filter locally — the next
section shows how to confirm that with `EXPLAIN`.

### Example 15 — Full scan of a prefix

```sql
SELECT key, value FROM app_config ORDER BY key;
--  key  |     value
-- ------+----------------
--  host | db.example.com
--  name | etcd_fdw
--  port | 5432
```

### Example 16 — Equality (single-key get, pushed down)

A `key = '...'` predicate becomes a single-key fetch from etcd — the cheapest
possible read, no scan.

```sql
SELECT value FROM app_config WHERE key = 'host';
--      value
-- ----------------
--  db.example.com
```

### Example 17 — Bounded range (pushed down)

A half-open byte range turns into one etcd Range request. Here `>= 'h'` and
`< 'p'` selects `host` and `name` but not `port`.

```sql
SELECT key FROM app_config WHERE key >= 'h' AND key < 'p' ORDER BY key;
--  key
-- ------
--  host
--  name
```

### Example 18 — Prefix match with LIKE (pushed down)

A `LIKE 'p%'` with the wildcard only at the end is recognised as a prefix scan.

```sql
SELECT key, value FROM app_config WHERE key LIKE 'p%';
--  key  | value
-- ------+-------
--  port | 5432
```

### Example 19 — Prefix match with the ^@ operator

The `^@` (starts-with) operator pushes down the same way and reads more clearly
than `LIKE`.

```sql
SELECT key FROM app_config WHERE key ^@ 'na';   -- keys starting with "na"
--  key
-- ------
--  name
```

### Example 20 — IN-list (multi-key fetch, pushed down)

An `IN` list becomes one etcd get per listed key. Values outside the table's
prefix are dropped automatically, and duplicates are removed.

```sql
SELECT key, value FROM app_config WHERE key IN ('host', 'port') ORDER BY key;
--  key  |     value
-- ------+----------------
--  host | db.example.com
--  port | 5432
```

### Example 21 — ORDER BY key, ascending and descending

etcd can return keys already sorted, so an `ORDER BY key` (either direction) is
satisfied by etcd and PostgreSQL does not add a Sort.

```sql
SELECT key FROM app_config ORDER BY key;        -- ASC pushed to etcd
SELECT key FROM app_config ORDER BY key DESC;   -- DESC pushed to etcd
```

### Example 22 — LIMIT pushed down

When there is no local filtering to do, a `LIMIT` is sent to etcd so only that
many keys cross the wire.

```sql
SELECT key FROM app_config ORDER BY key LIMIT 2;
--  key
-- ------
--  host
--  name
```

### Example 23 — Count keys under a prefix

```sql
SELECT count(*) FROM app_config;
--  count
-- -------
--      3
```

### Example 24 — Filter on a non-key column (rechecked locally)

There is no server-side index on values, so a predicate on `value` cannot push
down. The wrapper still scopes the scan to the prefix and PostgreSQL applies the
filter to the rows it gets back.

```sql
SELECT key FROM app_config WHERE value = '5432';
--  key
-- ------
--  port
```

### Example 25 — Whole-keyspace query scoped by prefix

Using the `etcd_all` table from Example 9, a `LIKE` predicate still pushes down
as a prefix scan even though the table covers everything.

```sql
SELECT key FROM etcd_all WHERE key LIKE '/app/config/%' ORDER BY key;
--          key
-- --------------------
--  /app/config/host
--  /app/config/name
--  /app/config/port
```

---

## 4. Inspecting pushdown

`EXPLAIN (VERBOSE)` shows exactly what the wrapper will ask etcd for. The
`etcd Scan` line tells you whether it is a single key, a range, a multi-key
fetch, or a provably-empty result, and a `Filter:` line appears for anything
rechecked locally.

### Example 26 — EXPLAIN a single-key lookup

```sql
EXPLAIN (VERBOSE, COSTS OFF) SELECT value FROM app_config WHERE key = 'host';
--  Foreign Scan on public.app_config
--    Output: value
--    etcd Endpoint: http://127.0.0.1:2379
--    etcd Prefix: /app/config/
--    etcd Scan: single key
--    etcd Order: ASC
```

### Example 27 — EXPLAIN range, multi-key and a local filter

Three plans side by side: a bounded range, an `IN` list (note `etcd Scan keys`),
and a value filter that stays local (note the `Filter:` line and the absence of
any value-based etcd scan).

```sql
EXPLAIN (VERBOSE, COSTS OFF)
  SELECT key FROM app_config WHERE key >= 'h' AND key < 'p';
--    etcd Scan: range

EXPLAIN (VERBOSE, COSTS OFF)
  SELECT key FROM app_config WHERE key IN ('host','port');
--    etcd Scan keys: 2

EXPLAIN (VERBOSE, COSTS OFF)
  SELECT key FROM app_config WHERE value = 'x';
--    Filter: (app_config.value = 'x'::text)
--    etcd Scan: range
```

---

## 5. Writing

The wrapper supports the full write surface. Remember the one big caveat: etcd
is not part of PostgreSQL's transaction, so writes are applied immediately and
are **not** rolled back by `ROLLBACK` (see [transactions.md](transactions.md)).

### Example 28 — Insert one or many rows

`INSERT` maps to an etcd `Put`. Note that this is an upsert: inserting an
existing key overwrites it rather than raising a duplicate-key error.

```sql
INSERT INTO app_config (key, value) VALUES ('timeout', '30');
INSERT INTO app_config (key, value)
  SELECT 'flag_' || g, 'on' FROM generate_series(1, 3) g;
```

### Example 29 — Update a value

When the table includes a `mod_revision` column, updates use an etcd transaction
guarded on that revision, giving optimistic concurrency (Example 42). Without
it, the update is unconditional (last writer wins).

```sql
UPDATE app_config SET value = '5433' WHERE key = 'port';
```

### Example 30 — Rename a key

Changing the `key` does a `Put` of the new key and a `Delete` of the old one in
a single etcd transaction, so the rename is atomic.

```sql
UPDATE app_config SET key = 'db_port' WHERE key = 'port';
SELECT key FROM app_config WHERE key LIKE '%port' ORDER BY key;
--    key
-- ---------
--  db_port
```

### Example 31 — Delete

A single-key `DELETE` is a `DeleteRange` of one key. A predicate that matches
several rows scans them first and deletes them one at a time.

```sql
DELETE FROM app_config WHERE key = 'timeout';
DELETE FROM app_config WHERE key LIKE 'flag_%';
```

### Example 32 — COPY into a foreign table

`COPY ... FROM` works and is the fastest way to bulk-load from a file or stream.

```sql
COPY app_config (key, value) FROM stdin;
k1	v1
k2	v2
\.
SELECT key, value FROM app_config WHERE key IN ('k1','k2') ORDER BY key;
--  key | value
-- -----+-------
--  k1  | v1
--  k2  | v2
```

### Example 33 — Bulk insert (coalesced into etcd transactions)

Multi-row inserts are batched: the wrapper groups rows into etcd transactions
(about 100 puts each, comfortably under etcd's default 128-op transaction
limit), which is far faster than one round trip per row.

```sql
INSERT INTO app_config (key, value)
  SELECT 'bulk_' || lpad(g::text, 5, '0'), 'v' || g
  FROM generate_series(1, 1000) g;
SELECT count(*) FROM app_config WHERE key LIKE 'bulk_%';
--  count
-- -------
--   1000
```

### Example 34 — TRUNCATE

`TRUNCATE` issues a single ranged delete over the whole table prefix — much
cheaper than deleting row by row.

```sql
TRUNCATE app_config;
SELECT count(*) FROM app_config;
--  count
-- -------
--      0
```

### Example 35 — Attach a lease to a key

A lease makes a key expire automatically. The lease must already exist. Note
the conversion: `etcdctl` prints the lease id in **hexadecimal**, but the
`lease` column is a `bigint`, so convert it to decimal first.

```bash
LHEX=$(etcdctl lease grant 60 | awk '{print $2}')   # e.g. 694d9eccd7b8bf28
LDEC=$(printf '%d' "0x$LHEX")                        # -> 7587895549818879784
```

```sql
-- the key is removed automatically when the lease expires
INSERT INTO app_config (key, value, lease)
  VALUES ('ephemeral', '1', 7587895549818879784);
```

Using a lease id that does not exist raises an error from etcd.

### Example 35b — Manage the lease lifecycle from SQL

From version 1.3 you can grant, inspect, refresh and revoke leases without
`etcdctl`. `etcd_fdw_lease_grant` returns the id directly as a `bigint`, so
there is no hex/decimal conversion to worry about.

```sql
-- grant a 60-second lease and attach a key to it
SELECT etcd_fdw_lease_grant('etcd', 60) AS lid \gset
INSERT INTO app_config (key, value, lease) VALUES ('ephemeral', '1', :lid);

SELECT etcd_fdw_lease_ttl('etcd', :lid);        -- remaining seconds (e.g. 58)
SELECT etcd_fdw_lease_keepalive('etcd', :lid);  -- refresh; returns the new TTL

-- revoking the lease deletes every key attached to it
SELECT etcd_fdw_lease_revoke('etcd', :lid);     -- true
SELECT etcd_fdw_lease_ttl('etcd', :lid);        -- -1 (gone)
```

### Example 36 — Store and read binary data

Using the `blobs` table from Example 8, `bytea` values round-trip exactly.

```sql
INSERT INTO blobs (key, value) VALUES ('icon', '\xdeadbeef');
SELECT key, value FROM blobs WHERE key = 'icon';
--  key  |   value
-- ------+------------
--  icon | \xdeadbeef
```

---

## 6. Joins & lookups

A common use is enriching local data with values held in etcd. The wrapper can
turn an equality join on the key into a per-row single-key lookup, which is far
cheaper than scanning the whole prefix when the local side is small.

### Example 37 — Parameterized join with a local table

```sql
CREATE TABLE wanted (k text COLLATE "C");
INSERT INTO wanted VALUES ('host'), ('port');

SELECT w.k, c.value
FROM wanted w JOIN app_config c ON c.key = w.k
ORDER BY w.k;
--   k   |     value
-- ------+----------------
--  host | db.example.com
--  port | 5432
```

### Example 38 — Correlated subquery lookup

The same single-key lookup, expressed as a scalar subquery. A non-matching key
yields `NULL` rather than dropping the row.

```sql
SELECT w.k, (SELECT value FROM app_config c WHERE c.key = w.k) AS value
FROM wanted w
ORDER BY w.k;
```

### Example 39 — Encourage the parameterized plan

The planner only picks the per-row lookup when it believes the foreign side is
large relative to the local side. Turning on `use_remote_estimate` lets it ask
etcd for an accurate row count; then `EXPLAIN` shows the parameterized plan.

```sql
ALTER FOREIGN TABLE app_config OPTIONS (ADD use_remote_estimate 'true');

EXPLAIN (VERBOSE, COSTS OFF)
  SELECT w.k, c.value FROM wanted w JOIN app_config c ON c.key = w.k;
--  Nested Loop
--    ->  Seq Scan on wanted w
--    ->  Foreign Scan on app_config c
--          etcd Scan: parameterized key
```

When a plain (non-parameterized) plan is cheaper, the planner uses that instead;
both produce the same result.

---

## 7. Schema import & statistics

### Example 40 — IMPORT FOREIGN SCHEMA from a key hierarchy

`IMPORT FOREIGN SCHEMA` treats the immediate "directories" under a prefix (keys
split on `/`) as tables and creates one foreign table for each, with the
standard column set.

```bash
etcdctl put /svc/auth/host   auth.internal
etcdctl put /svc/auth/port   8443
etcdctl put /svc/cache/host  cache.internal
```

```sql
CREATE SCHEMA svc;
IMPORT FOREIGN SCHEMA "/svc/" FROM SERVER etcd INTO svc;
-- creates svc.auth and svc.cache

SELECT key, value FROM svc.auth ORDER BY key;
--  key  |     value
-- ------+---------------
--  host | auth.internal
--  port | 8443
```

### Example 41 — ANALYZE for planner statistics

`ANALYZE` samples the prefix and records a row estimate, which improves planning
(especially for joins). Run it after large changes.

```sql
ANALYZE app_config;
SELECT reltuples::int FROM pg_class WHERE relname = 'app_config';
```

---

## 8. Concurrency & integration patterns

### Example 42 — Safe concurrent increment (optimistic retry)

With a `mod_revision` column, two writers that race on the same key will not
both succeed: the loser's transaction fails its revision check and raises
SQLSTATE `40001`. Catch it and retry, and concurrent increments never lose an
update. (This is exactly what `test/scripts/run-concurrency.sh` verifies under
load — N workers, exact final total.)

```sql
DO $$
BEGIN
  LOOP
    BEGIN
      UPDATE app_config
         SET value = ((value::int) + 1)::text
       WHERE key = 'counter';
      EXIT;                              -- success
    EXCEPTION WHEN serialization_failure THEN
      -- another writer won the race; re-read and retry
    END;
  END LOOP;
END $$;
```

### Example 43 — A typed view over raw KV

etcd stores strings, but you can present a clean, typed surface to your
application with a view that pulls named keys and casts them.

```sql
CREATE VIEW config_typed AS
SELECT
  (SELECT value FROM app_config WHERE key = 'host')      AS host,
  (SELECT value FROM app_config WHERE key = 'port')::int AS port;

SELECT * FROM config_typed;
--       host       | port
-- ----------------+------
--  db.example.com | 5432
```

### Example 44 — Join etcd service registry against local data

Treat etcd as a live service registry and join it to your own tables.

```sql
SELECT s.name, e.value AS host
FROM my_services s
JOIN svc.auth e ON e.key = 'host'
WHERE s.name = 'auth';
```

### Example 45 — Cache an etcd snapshot in a materialized view

When you want a stable, indexable local copy (and can tolerate it being
slightly stale), materialize it and refresh on demand.

```sql
CREATE MATERIALIZED VIEW config_snapshot AS
  SELECT key, value FROM app_config;

REFRESH MATERIALIZED VIEW config_snapshot;   -- re-pull from etcd
```

### Example 46 — Multiple tables sharing one server prefix

A server-level `prefix` is prepended to every table's prefix, which is a tidy
way to namespace an environment.

```sql
CREATE SERVER etcd_env FOREIGN DATA WRAPPER etcd_fdw
  OPTIONS (endpoints 'http://127.0.0.1:2379', prefix '/prod/');

CREATE FOREIGN TABLE prod_db (key text COLLATE "C", value text)
  SERVER etcd_env OPTIONS (prefix 'db/', strip_prefix 'true');
-- effective prefix: /prod/db/
```

---

## 9. TLS certificate monitoring (v1.1+)

When you connect over TLS you will want to know, from SQL, when certificates
expire — both the one etcd presents and the local CA/client files you
configured. Three objects make that possible; see [security.md](security.md)
for the access rules (superuser, `pg_monitor`, or `USAGE` on the server).

### Example 47 — List all certificates for a server

`source` distinguishes the live peer certificate (`peer`, plus `peer-chain` for
intermediates) from the files on disk (`cafile`, `certfile`).

```sql
SELECT source, subject, not_after, expires_in
FROM etcd_fdw_certificates('etcd_tls');
--  source |      subject       |       not_after        | expires_in
-- --------+--------------------+------------------------+------------
--  peer   | CN = etcd.internal | 2028-09-18 05:24:47+00 | 824 days...
--  cafile | /CN=etcd-ca        | 2036-06-13 11:00:00+00 | ...
```

### Example 48 — The etcd server cert's expiry as a single value

```sql
SELECT etcd_fdw_server_cert_expiry('etcd_tls');
--    etcd_fdw_server_cert_expiry
-- -------------------------------
--  2028-09-18 05:24:47+00
```

### Example 49 — Alert on certs expiring within 30 days (all servers)

The `etcd_fdw_cert_expiry` view applies the functions across every etcd server,
which is what you want for a single monitoring query.

```sql
SELECT server, source, subject, not_after
FROM etcd_fdw_cert_expiry
WHERE not_after < now() + interval '30 days'
ORDER BY not_after;
```

### Example 50 — A monitoring role that can see every server's certs

Grant `pg_monitor` for cluster-wide visibility, or `USAGE` on one server for a
narrow grant.

```sql
CREATE ROLE certmon LOGIN;
GRANT pg_monitor TO certmon;            -- may inspect all etcd_fdw servers
-- or, for a single server only:
-- GRANT USAGE ON FOREIGN SERVER etcd_tls TO certmon;
```

---

## 10. High availability & certificate rotation

These tie together the resilience and TLS features for day-2 operations. See
[high-availability.md](high-availability.md) for the full failover model.

### Example 51 — Multiple endpoints with failover and retry tuning

Lower `connect_timeout_ms` to fail over to a healthy node quickly; raise
`max_retries`/`retry_backoff_ms` if your cluster has longer leader elections.

```sql
CREATE SERVER etcd FOREIGN DATA WRAPPER etcd_fdw OPTIONS (
  endpoints          'https://etcd1:2379,https://etcd2:2379,https://etcd3:2379',
  use_tls            'true',
  cafile             '/etc/pki/etcd/ca.crt',
  connect_timeout_ms '1500',   -- fail over quickly
  max_retries        '3',      -- ride out a leader election
  retry_backoff_ms   '200'
);
```

### Example 52 — Rotate certificates with no downtime

If you replace files at the same paths, new connections pick them up; force the
current backend to refresh with `etcd_fdw_disconnect()`, or force every backend
cluster-wide with `ALTER SERVER`. For a CA migration, point `cafile` at a bundle
containing both the old and new CA, reissue the server cert, then drop the old
CA.

```bash
cp new-ca.crt /etc/pki/etcd/ca.crt        # replace in place
```

```sql
-- this backend, immediately:
SELECT etcd_fdw_disconnect();

-- every backend, immediately (catalog invalidation forces reconnect):
ALTER SERVER etcd OPTIONS (SET cafile '/etc/pki/etcd/ca.crt');
```

### Example 53 — A single dashboard query for operations

One query that surfaces anything needing attention across all etcd servers.

```sql
SELECT server, source, subject, not_after,
       (not_after < now() + interval '30 days') AS expiring_soon
FROM etcd_fdw_cert_expiry
ORDER BY not_after;
```

---

See also: [high-availability.md](high-availability.md),
[reference.md](reference.md) (every option and function),
[data-model.md](data-model.md), [pushdown.md](pushdown.md),
[transactions.md](transactions.md), [security.md](security.md).

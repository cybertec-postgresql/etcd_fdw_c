# etcd_fdw

A PostgreSQL **foreign-data wrapper for [etcd](https://etcd.io) v3**, written in C.
It exposes an etcd key/value store as foreign tables so you can read and write
etcd data with plain SQL, push key predicates down to etcd, and join etcd-backed
configuration / service data against ordinary PostgreSQL tables.

- **Read** with predicate pushdown: `=`, range (`<`, `<=`, `>`, `>=`), prefix
  (`LIKE 'p%'`, `^@`), `IN (...)`, `ORDER BY key`, and `LIMIT` are translated into
  etcd `Range` requests where it is safe to do so.
- **Joins**: `key = outer.col` joins can run as parameterized nested loops, one
  etcd key lookup per outer row.
- **Write**: `INSERT` (with batching), `UPDATE` (including key rename), `DELETE`,
  `COPY`, and `TRUNCATE`, with optimistic concurrency via etcd transactions
  guarded on `mod_revision`.
- `IMPORT FOREIGN SCHEMA` to auto-create tables from etcd key prefixes.
- `ANALYZE` support for planner statistics.
- **Lease management** from SQL: grant, ttl, keepalive, revoke.
- **TLS** and **etcd RBAC authentication** (token, via user mappings), with
  **certificate-expiry monitoring** functions and a view.
- **High availability**: multiple `endpoints` with automatic failover (on
  transport errors and HTTP 5xx) and bounded retries to ride out leader
  elections.
- Talks to etcd's HTTP/JSON gRPC-gateway with **libcurl** — the only external
  dependency. No gRPC toolchain required.

Supported: **PostgreSQL 14–18**, **etcd 3.4 / 3.5 / 3.6** (tested in CI across
the full matrix, including TLS and auth).

## Quick start

```sql
CREATE EXTENSION etcd_fdw;

CREATE SERVER etcd
  FOREIGN DATA WRAPPER etcd_fdw
  OPTIONS (endpoints 'http://127.0.0.1:2379');

-- Map the key prefix /app/config/ to a table.
CREATE FOREIGN TABLE app_config (
  key             text COLLATE "C",   -- "C" enables range/prefix/ORDER BY pushdown
  value           text,
  create_revision bigint,
  mod_revision    bigint,
  version         bigint,
  lease           bigint
) SERVER etcd
  OPTIONS (prefix '/app/config/', strip_prefix 'true');

INSERT INTO app_config (key, value) VALUES ('host', 'db.example.com'), ('port', '5432');

SELECT key, value FROM app_config WHERE key = 'host';
SELECT key, value FROM app_config WHERE key LIKE 'p%';
UPDATE app_config SET value = '5433' WHERE key = 'port';
DELETE FROM app_config WHERE key = 'host';
```

## Build & install

Requires the PostgreSQL server development headers plus libcurl and OpenSSL
(libcrypto) development headers:

```bash
# Debian/Ubuntu, for the PostgreSQL you are targeting:
sudo apt-get install build-essential postgresql-server-dev-16 \
  libcurl4-openssl-dev libssl-dev

make
sudo make install            # uses pg_config on PATH; override with PG_CONFIG=...
```

Then `CREATE EXTENSION etcd_fdw;` in your database.

## Documentation

- [Examples & cookbook](doc/examples.md) — 50+ worked examples
- [Installation](doc/installation.md)
- [High availability & resilience](doc/high-availability.md)
- [Option reference](doc/reference.md)
- [Data model](doc/data-model.md)
- [Predicate pushdown](doc/pushdown.md)
- [Transactions & concurrency](doc/transactions.md)
- [Security: TLS & authentication](doc/security.md)
- [Limitations](doc/limitations.md)
- [Troubleshooting](doc/troubleshooting.md)
- [Testing](doc/testing.md) — what the suite checks and how it works

## Testing

The suite runs in containers across the full version matrix:

```bash
# one cell
bash test/scripts/run-cell.sh 16 v3.5.16
# the whole matrix (PG 14-18 x etcd 3.4/3.5/3.6)
bash test/scripts/run-matrix.sh
# auth + TLS checks
bash test/scripts/run-auth-tls.sh 16 v3.6.1
# concurrency (optimistic-locking / lost-update) checks
bash test/scripts/run-concurrency.sh 16 v3.6.1
# high-availability multi-endpoint failover checks
bash test/scripts/run-ha.sh 16 v3.6.1
# AddressSanitizer build + full suite
bash test/scripts/run-asan.sh 16 v3.6.1
```

See [doc/testing.md](doc/testing.md) for a full explanation of every suite and
hardening job, and [test/README.md](test/README.md) for the command cheat-sheet.

## License

PostgreSQL License. See [LICENSE](LICENSE). Bundles [cJSON](https://github.com/DaveGamble/cJSON)
(MIT) under `third_party/cJSON/`.

---

CYBERTEC PostgreSQL International GmbH, https://www.cybertec-postgresql.com

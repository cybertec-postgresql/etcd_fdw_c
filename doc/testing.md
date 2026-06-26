# Testing

This page explains what the `etcd_fdw` test suite checks and how it works: the
correctness suites, the version matrix, and the hardening jobs (sanitizers,
fuzzing, fault injection, failover and concurrency). For a terse command
cheat-sheet see [test/README.md](../test/README.md); this page is the narrative
reference.

## How testing works

Everything runs **inside Docker**. You do not need PostgreSQL or libcurl
development headers on the host — each test spins up a stock `postgres:<major>`
container, builds the extension there, and exercises it against a real `etcd`
container (or, for one job, a mock). This is also why the host in this repository
is not set up to compile: the containers are the build environment.

A few invariants hold across all scripts:

- **Throwaway containers on a private network.** Each run creates its own Docker
  network and containers and tears them down on exit (even on failure), so runs
  do not collide and leave nothing behind.
- **The etcd service is reachable as `http://etcd:2379`.** The regression SQL
  hardcodes that URL; the scripts attach the etcd container to the network under
  the `etcd` alias so the hardcoded URL resolves.
- **Isolated builds.** Inside the container the mounted source is copied to a
  fresh `/build` directory before compiling, so several cells building
  concurrently never share object files.
- **Health-gated startup.** `wait-for-etcd.sh` polls `etcdctl endpoint health`
  before any SQL runs, so tests never race a still-starting cluster.

## Correctness suites (pg_regress)

The standard regression suites are listed in the `Makefile`:

```makefile
REGRESS = basic pushdown dml join lease import analyze errors
REGRESS_OPTS = --inputdir=test --outputdir=test --load-extension=etcd_fdw
```

Each suite is a `test/sql/<name>.sql` script whose output is diffed against
`test/expected/<name>.out`. Actual output lands in `test/results/` (git-ignored).

| Suite | What it checks |
|---|---|
| `basic` | Server/table DDL and option validation; simple reads and writes; `strip_prefix`; `bytea` value columns; the revision columns (`create_revision`, `mod_revision`, `version`); full-key exposure; whole-keyspace tables. |
| `pushdown` | Predicate pushdown to a single etcd `Range`: equality (single-key get), bounded ranges, `LIKE 'p%'` / `^@` prefix, `key IN (...)` multi-key fetch, `ORDER BY key` and `LIMIT`; plus local recheck of non-key predicates and `EXPLAIN` shape. |
| `dml` | `INSERT`/`UPDATE`/`DELETE`, key rename (an `UPDATE` of `key`), `COPY FROM`, batch insert across the 100-op boundary, and `TRUNCATE`. |
| `join` | Parameterized foreign scans: `key = outer.col` nested-loop joins and correlated subqueries, with remote estimates and forced nested-loop plans. |
| `lease` | The lease lifecycle: `etcd_fdw_lease_grant`, `_ttl`, `_keepalive`, `_revoke`, attaching a key to a lease via the `lease` column, and revoke cascading to the attached keys. |
| `import` | `IMPORT FOREIGN SCHEMA` discovering one table per immediate child prefix, then querying the generated tables. |
| `analyze` | `ANALYZE` populating row estimates from etcd key counts. |
| `errors` | Negative paths: bogus options, non-positive timeouts, an unreachable endpoint failing fast, and tables without a `key` column being non-updatable. |

TLS and etcd RBAC need a specially configured cluster, so they are not part of
the pg_regress matrix; they run as a dedicated job (see `run-auth-tls.sh` below).

## A single cell

```bash
bash test/scripts/run-cell.sh <pg_major> <etcd_tag>
bash test/scripts/run-cell.sh 16 v3.5.16
```

One cell is one (PostgreSQL major) × (etcd version) combination. The script:

1. creates a private network and starts an `etcd` container (alias `etcd`);
2. waits for it to report healthy;
3. starts a `postgres:<pg_major>` container with the repo mounted;
4. inside it, installs build deps, copies source to `/build`, compiles and
   installs the extension;
5. runs `make installcheck` (the pg_regress suites above);
6. prints `test/regression.diffs` on failure;
7. tears everything down.

Exit code `0` means every suite passed. Set `ETCD_IMAGE` to override the etcd
image (default `quay.io/coreos/etcd:<etcd_tag>`).

## The version matrix

```bash
bash test/scripts/run-matrix.sh
PG_VERSIONS="16 17" ETCD_TAGS="v3.6.1" bash test/scripts/run-matrix.sh
```

`run-matrix.sh` runs `run-cell.sh` for the full cross product. Defaults:

- `PG_VERSIONS="14 15 16 17 18"`
- `ETCD_TAGS="v3.4.34 v3.5.16 v3.6.1"`

That is 5 × 3 = **15 cells**. It prints `matrix complete: N passed, M failed`
and lists any failing `pg<major>/<etcd_tag>` cells. Override either list via the
environment to narrow a run.

## Hardening jobs

These run outside the pg_regress matrix and target specific failure classes.
Each takes `[pg_major] [etcd_tag]` positional arguments (defaulting to
`16 v3.6.1`) unless noted, and honours `ETCD_IMAGE`.

### Memory: AddressSanitizer

```bash
bash test/scripts/run-asan.sh 16 v3.6.1
```

Builds the extension with `-fsanitize=address` and runs the full pg_regress
suite with postgres started under `libasan`. Fails if ASan reports any memory
error (use-after-free, overflow, bad free). This is the primary detector for the
extension's memory handling.

### Memory: Valgrind memcheck

```bash
bash test/scripts/run-valgrind.sh 16 v3.6.1
```

Runs a single-user backend under Valgrind through a script that exercises the hot
paths — DDL, a 60-row `INSERT ... SELECT` (the batch path), pushdown variants,
an `UPDATE` with key rename, `DELETE`, the lease functions, `IMPORT FOREIGN
SCHEMA`, `ANALYZE` and `TRUNCATE`. Valgrind always reports some noise from inside
libcurl/OpenSSL/glibc, so the script **filters errors to extension frames**
(`etcd_client`, `etcd_conn`, `etcd_json`, `deparse`, `options.c`, `etcd_cert`,
`etcd_lease`, `etcd_fdw`) and passes only if none of those appear.

### Parser robustness: JSON fuzzing

```bash
bash test/scripts/run-jsonfuzz.sh 16 300   # second arg = iterations
```

Points the extension at `test/docker/mock_etcd.py`, a mock gateway that answers
every request with malformed/adversarial JSON — empty bodies, truncated objects,
wrong field types, invalid base64, huge numbers, deeply nested objects, binary
garbage, plus random truncation/bit-flips/junk appended to each response. The
backend (built with ASan) must reject every response cleanly and stay alive: the
final `SELECT 1` liveness check must succeed and ASan must be silent.

### Resilience: HTTP 5xx fault injection

```bash
bash test/scripts/run-fault.sh 16 v3.6.1
```

Puts an nginx container that returns `503` for everything in front of a real
etcd. It verifies (1) a server whose first endpoint is the 503 node fails over to
the healthy node and returns correct data, and (2) a server pointing only at the
503 node errors cleanly with "no etcd endpoint available" after `max_retries` —
never returning the 503 and never hanging.

### Resilience: multi-endpoint high availability

```bash
bash test/scripts/run-ha.sh 16 v3.6.1
```

Runs two independent etcd nodes behind one multi-endpoint server and walks
through: both up; an `ALTER SERVER` mid-session (must invalidate the cached
connection and reconnect without a use-after-free); the primary node killed
(fail over to the second); both down (clean error, no hang); and recovery
(queries succeed again once a node returns).

### Correctness under contention: concurrency

```bash
bash test/scripts/run-concurrency.sh 16 v3.6.1 8 50   # workers, iters
```

Spawns concurrent backends to confirm there are no lost updates and no crashes.
The lost-update test has N workers each perform M `mod_revision`-guarded
increments (retrying on `serialization_failure`); the final counter must equal
exactly `workers × iters`. A second test has each worker insert a disjoint block
of rows and checks the exact total. PostgreSQL logs are scanned for any crash
signature.

### Auth and TLS

```bash
bash test/scripts/run-auth-tls.sh 16 v3.6.1
```

The RBAC and TLS path, which needs a specially configured etcd. It:

- enables etcd RBAC, then reads/writes through a `USER MAPPING` with
  `username`/`password`, including an auth **failover** case where the token is
  fetched from a live node when the first endpoint is dead;
- generates a CA and a SAN server certificate (see *Certificates* below), starts
  etcd with TLS, and reads/writes over `use_tls='true'` with a `cafile`;
- exercises the certificate-monitoring surface — `etcd_fdw_certificates()` (peer
  and `cafile` rows, subject, future expiry) and the `etcd_fdw_cert_expiry`
  view;
- checks the privilege guard: an unprivileged role is denied the certificate
  functions.

## Docker assets

- **`test/docker/Dockerfile`** — builds the extension on `postgres:${PG_MAJOR}`
  (default 16) with `libcurl4-openssl-dev` and `libssl-dev`. Used by the local
  dev compose stack.
- **`test/docker/docker-compose.yml`** — a one-etcd + one-PostgreSQL stack for
  local iteration (see below).
- **`test/docker/mock_etcd.py`** — the malformed-JSON gateway used by the fuzz
  job.

## Certificates

`test/certs/` holds TLS material (git-ignored): a CA key/cert, a server
key/CSR/cert, and `san.cnf` (subject `CN=etcd_tls`, SANs `etcd_tls`,
`localhost`, `127.0.0.1`). `run-auth-tls.sh` regenerates this material in a temp
directory mounted into both containers, so the checked-in files are only a
convenience — the job does not depend on them.

## Local development environment

For fast iteration without the per-run container churn:

```bash
PG_MAJOR=16 ETCD_TAG=v3.5.16 \
  docker compose -f test/docker/docker-compose.yml up --build -d
docker compose -f test/docker/docker-compose.yml exec -u postgres pg \
  make -C /src installcheck
docker compose -f test/docker/docker-compose.yml down -v
```

## Continuous integration

CI runs the same scripts, so a green local matrix matches a green CI run. The
workflow covers the PG × etcd matrix plus the hardening jobs (ASan, Valgrind,
JSON fuzzing, fault injection, HA failover, concurrency, and auth/TLS).

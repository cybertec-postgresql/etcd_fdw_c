# Tests

The suite runs entirely in Docker, building `etcd_fdw` against a target
PostgreSQL and exercising it against a real etcd.

## Layout

```
sql/         pg_regress input scripts
expected/    expected output (diffed by pg_regress)
results/     actual output (written by pg_regress; git-ignored)
docker/      Dockerfile, docker-compose.yml, mock_etcd.py (fuzz target)
scripts/     run-cell.sh, run-matrix.sh, run-asan.sh, run-valgrind.sh,
             run-auth-tls.sh, run-concurrency.sh, run-ha.sh, run-fault.sh,
             run-jsonfuzz.sh, wait-for-etcd.sh
certs/       generated TLS material for manual TLS testing (git-ignored)
```

## Running

```bash
# one cell: build against PG 16, test against etcd v3.5.16
bash test/scripts/run-cell.sh 16 v3.5.16

# the whole matrix (override the lists via env)
bash test/scripts/run-matrix.sh
PG_VERSIONS="16 17" ETCD_TAGS="v3.6.1" bash test/scripts/run-matrix.sh

# AddressSanitizer build + full suite
bash test/scripts/run-asan.sh 16 v3.6.1
# concurrency: lost-update + parallel-insert checks
bash test/scripts/run-concurrency.sh 16 v3.6.1
# high availability: multi-endpoint failover / recovery
bash test/scripts/run-ha.sh 16 v3.6.1
# Valgrind memcheck of the extension's hot paths
bash test/scripts/run-valgrind.sh 16 v3.6.1
# fault injection: HTTP 5xx failover / retry
bash test/scripts/run-fault.sh 16 v3.6.1
# JSON fuzzing (malformed responses) under ASan
bash test/scripts/run-jsonfuzz.sh 16 300
```

`run-jsonfuzz.sh` uses `test/docker/mock_etcd.py`, a mock gateway that returns
malformed/adversarial JSON; the backend must error cleanly and never crash.

Each cell creates a throwaway Docker network with an `etcd` service (reachable as
`http://etcd:2379`, which the regression SQL hardcodes) and a PostgreSQL
container that builds and installs the extension, then runs `make installcheck`.
Everything is torn down on exit.

## Local development environment

```bash
PG_MAJOR=16 ETCD_TAG=v3.5.16 docker compose -f test/docker/docker-compose.yml up --build -d
docker compose -f test/docker/docker-compose.yml exec -u postgres pg make -C /src installcheck
docker compose -f test/docker/docker-compose.yml down -v
```

## Suites

| Suite | Covers |
|---|---|
| `basic` | server/table DDL, reads, `strip_prefix`, `bytea` values |
| `pushdown` | eq/range/prefix/IN/ORDER BY/LIMIT pushdown, local recheck, `EXPLAIN` |
| `dml` | INSERT/UPDATE/DELETE, key rename, COPY, batch insert, TRUNCATE |
| `join` | parameterized `key = outer.col` joins and correlated subqueries |
| `lease` | lease grant / ttl / keepalive / revoke and the `lease` column |
| `import` | `IMPORT FOREIGN SCHEMA` |
| `analyze` | `ANALYZE` row estimates |
| `errors` | option validation, unreachable endpoint, non-updatable table |

TLS and etcd RBAC authentication need a specially configured etcd, so they run
outside the pg_regress matrix via:

```bash
bash test/scripts/run-auth-tls.sh 16 v3.6.1
```

which is also a CI job. See [doc/security.md](../doc/security.md) for the manual
procedure.

For a full explanation of what every suite and script checks, how a test cell is
built and torn down, and how the hardening jobs (sanitizers, fuzzing, fault
injection, failover, concurrency) work, see
[doc/testing.md](../doc/testing.md).

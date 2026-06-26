# Development guide

## What this is

`etcd_fdw` — a PostgreSQL foreign-data wrapper for etcd v3, in C. It talks to
etcd's HTTP/JSON gRPC-gateway via libcurl (no gRPC toolchain). See `README.md`
and `doc/` for behaviour.

## Build & test

The extension is built and tested **inside Docker**, so you don't need the
PostgreSQL/libcurl development headers on the host.

```bash
# build + regression on one PG x etcd cell
bash test/scripts/run-cell.sh 16 v3.6.1
# whole matrix: PG 14-18 x etcd 3.4/3.5/3.6
bash test/scripts/run-matrix.sh
# AddressSanitizer build + full suite (memory checking)
bash test/scripts/run-asan.sh 16 v3.6.1
```

The regression SQL connects to etcd at `http://etcd:2379`; the scripts provide
that hostname on a throwaway Docker network. See [doc/testing.md](doc/testing.md)
for what each suite and hardening job checks and how a cell is built.

To build directly on the host instead, install the dev headers and use PGXS:

```bash
sudo apt-get install build-essential postgresql-server-dev-16 \
  libcurl4-openssl-dev libssl-dev
make && sudo make install
```

## Code map

- `src/etcd_fdw.c` — FdwRoutine (scan + modify callbacks, import, analyze)
- `src/deparse.c` — key-predicate pushdown
- `src/etcd_client.c` — range/put/delete/txn/status over the gateway
- `src/etcd_conn.c` — libcurl transport, conn cache, auth, TLS, failover
- `src/etcd_json.c` — base64 + cJSON helpers (cJSON runs on palloc hooks)
- `src/etcd_cert.c` — TLS certificate inspection functions and the expiry view
- `src/etcd_lease.c` — lease grant / ttl / keepalive / revoke functions
- `src/options.c` — option parsing/validation
- `src/compat.h` — **all** PostgreSQL 14-18 version shims live here

## Rules of thumb

- Keep every `PG_VERSION_NUM` conditional in `compat.h`.
- FDW callbacks use the `etcd_client` API only — never libcurl/JSON directly.
- Never `PG_TRY`/`FlushErrorState` to swallow errors in planner callbacks; use
  missing-ok syscache lookups (see `etcd_get_options` for the user-mapping probe).
- Keep regression output deterministic: don't select cluster-global revisions.

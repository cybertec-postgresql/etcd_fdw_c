# Contributing

## Source layout

```
src/
  etcd_fdw.c     FdwRoutine: handler, validator, scan & modify callbacks,
                 IMPORT FOREIGN SCHEMA, ANALYZE
  deparse.c      WHERE-clause -> etcd Range translation (pushdown)
  etcd_client.c  high-level etcd ops: range / put / delete / txn / status
  etcd_conn.c    libcurl transport, connection cache, auth token, TLS, failover
  etcd_json.c    base64 + cJSON helpers (cJSON runs on palloc hooks)
  etcd_cert.c    TLS certificate inspection functions and the expiry view
  etcd_lease.c   lease grant / ttl / keepalive / revoke functions
  options.c      option parsing and validation
  compat.h       PostgreSQL 14-18 API shims (single home for version #ifdefs)
third_party/cJSON vendored JSON library (MIT)
test/            pg_regress suites, Docker matrix, scripts
doc/             documentation
```

## Conventions

- Match PostgreSQL's C style: tabs for indentation, `pgindent`-compatible
  formatting, declarations at the top of blocks.
- Keep all `PG_VERSION_NUM` conditionals in `src/compat.h`. Do not sprinkle
  version checks through the code.
- The FDW callbacks talk only to the `etcd_client` API, never to libcurl/JSON
  directly.
- Do not catch-and-ignore errors (`PG_TRY`/`FlushErrorState`) inside planner
  callbacks; probe catalogs with missing-ok syscache lookups instead.

## Building and testing

See [test/README.md](test/README.md). At minimum, before sending a change:

```bash
bash test/scripts/run-cell.sh 16 v3.6.1     # build + regression on one cell
bash test/scripts/run-asan.sh 16 v3.6.1     # AddressSanitizer build + suite
```

For changes that affect planning or version-specific APIs, run the full matrix
(`test/scripts/run-matrix.sh`) or rely on CI, which covers PG 14-18 × etcd
3.4/3.5/3.6.

## Updating expected test output

`pg_regress` writes actual output to `test/results/*.out`. After confirming a
change is correct, copy the relevant file to `test/expected/`. Keep test output
deterministic — avoid selecting cluster-global revision values directly.

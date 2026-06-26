# Installation

## Dependencies

- PostgreSQL **14–18** server development headers (`pg_config`, `postgres.h`).
- **libcurl** development headers (`curl/curl.h`).
- **OpenSSL** (`libcrypto`) development headers — used to parse local CA/client
  certificate files for the certificate-monitoring functions.
- A C compiler and `make`.
- [cJSON](https://github.com/DaveGamble/cJSON) is **vendored** under
  `third_party/cJSON/`; nothing to install.

On Debian/Ubuntu for a given major version `N`:

```bash
sudo apt-get install build-essential postgresql-server-dev-N \
  libcurl4-openssl-dev libssl-dev
```

On RHEL/Rocky:

```bash
sudo dnf install gcc make postgresqlNN-devel libcurl-devel openssl-devel
```

At **runtime** the database server needs the matching shared libraries —
`libcurl` and `libcrypto` (OpenSSL) — installed; the extension fails to load
without them.

## Build & install

The build uses PGXS and the `pg_config` on your `PATH`:

```bash
make
sudo make install
```

To build against a specific PostgreSQL when several are installed:

```bash
make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config
sudo make PG_CONFIG=/usr/lib/postgresql/17/bin/pg_config install
```

## Enable the extension

```sql
CREATE EXTENSION etcd_fdw;
```

This registers the `etcd_fdw` foreign-data wrapper, its handler and validator.

## Upgrading

After installing a newer build, bring an existing database up to the latest
extension version with:

```sql
ALTER EXTENSION etcd_fdw UPDATE;
```

Upgrade scripts are shipped for every step, so this also works across several
versions at once (for example 1.0 → 1.3). What each version adds:

- **1.1** — TLS certificate monitoring: `etcd_fdw_certificates()`,
  `etcd_fdw_server_cert_expiry()` and the `etcd_fdw_cert_expiry` view.
- **1.2** — resilience options (`max_retries`, `retry_backoff_ms`) and
  `etcd_fdw_disconnect()`.
- **1.3** — lease management: `etcd_fdw_lease_grant()`, `etcd_fdw_lease_ttl()`,
  `etcd_fdw_lease_keepalive()` and `etcd_fdw_lease_revoke()`.

## Verify

```sql
CREATE SERVER etcd FOREIGN DATA WRAPPER etcd_fdw
  OPTIONS (endpoints 'http://127.0.0.1:2379');
CREATE FOREIGN TABLE t (key text COLLATE "C", value text)
  SERVER etcd OPTIONS (prefix '/', strip_prefix 'true');
SELECT count(*) FROM t;
```

## Container builds

The repository ships a Dockerfile and compose file that build against any
supported PostgreSQL — see [test/README.md](../test/README.md). These are the
same paths exercised by CI.

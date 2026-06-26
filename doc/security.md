# Security: TLS & authentication

## TLS

Point the server at `https://` endpoints and enable TLS:

```sql
CREATE SERVER etcd FOREIGN DATA WRAPPER etcd_fdw OPTIONS (
  endpoints  'https://etcd.internal:2379',
  use_tls    'true',
  cafile     '/etc/pki/etcd/ca.crt'
);
```

- `cafile` — CA bundle used to verify the etcd server certificate. The server
  certificate's SAN must match the host in `endpoints`.
- `tls_verify 'false'` disables certificate/hostname verification. Use only in
  testing; it removes protection against man-in-the-middle attacks.

### Mutual TLS

If etcd requires client certificates, also set:

```sql
OPTIONS (..., certfile '/etc/pki/etcd/client.crt', keyfile '/etc/pki/etcd/client.key')
```

The certificate/key files are read by the PostgreSQL backend process and must be
readable by the `postgres` OS user.

## etcd RBAC authentication

When etcd has authentication enabled, supply credentials through a user mapping:

```sql
CREATE USER MAPPING FOR app_role SERVER etcd
  OPTIONS (username 'app', password 's3cret');
```

`etcd_fdw` authenticates lazily against `/v3/auth/authenticate`, caches the
returned token per backend connection, sends it in the `Authorization` header,
and transparently re-authenticates once on an HTTP 401.

Map different PostgreSQL roles to different etcd users to scope access according
to etcd's role-based permissions. Credentials live in the catalog like any other
user-mapping option; restrict who can see them with the usual privileges.

## Connection caching and DDL

Connections are cached per `(server, user)` within a backend. `ALTER`/`DROP` of
a server or user mapping invalidates the cache (via syscache callbacks), so
credential or endpoint changes take effect without reconnecting.

## Certificate & key management

### File layout and permissions
`cafile`, `certfile`, and `keyfile` are read by the PostgreSQL **backend
process**, so they must be readable by the `postgres` OS user. Keep the private
key tight:
```bash
install -o postgres -g postgres -m 0644 ca.crt   /etc/pki/etcd/ca.crt
install -o postgres -g postgres -m 0644 client.crt /etc/pki/etcd/client.crt
install -o postgres -g postgres -m 0600 client.key /etc/pki/etcd/client.key
```
Never expose the key elsewhere — `etcd_fdw` never reads or prints `keyfile`
contents (the cert functions below parse only the public `cafile`/`certfile`).

### Generating a CA, server, and client certificate
```bash
# CA
openssl genrsa -out ca.key 4096
openssl req -x509 -new -nodes -key ca.key -days 3650 -subj "/CN=etcd-ca" -out ca.crt

# etcd server cert (SAN must match the endpoint host)
cat > san.cnf <<'EOF'
[req]
distinguished_name = dn
req_extensions = v3_req
prompt = no
[dn]
CN = etcd.internal
[v3_req]
subjectAltName = DNS:etcd.internal,IP:10.0.0.10
EOF
openssl genrsa -out server.key 2048
openssl req -new -key server.key -subj "/CN=etcd.internal" -config san.cnf -out server.csr
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 825 \
  -extensions v3_req -extfile san.cnf -out server.crt

# optional client cert for mutual TLS
openssl genrsa -out client.key 2048
openssl req -new -key client.key -subj "/CN=pg-client" -out client.csr
openssl x509 -req -in client.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 825 -out client.crt
```

### Rotation
The FDW reads the configured cert **paths** on each TLS handshake, so the
mechanics depend on what you change:

- **Replace files in place (same paths).** New backends use the new files
  immediately. Existing backends pick them up on their next reconnect; a
  kept-alive idle connection keeps using the old session until it drops. To
  force an immediate refresh in the current backend, call
  `SELECT etcd_fdw_disconnect();` (drops this backend's cached connections so the
  next query re-reads the files).
- **Change a path or any option.** Run `ALTER SERVER etcd OPTIONS (SET cafile
  '...')`. This invalidates cached connections **cluster-wide** (via syscache
  invalidation), so every backend reconnects with the new configuration on its
  next query — verified: a session goes from failing (wrong CA) to succeeding
  (right CA) immediately after the `ALTER`. Even setting an option to its current
  value works, because `ALTER` always bumps the catalog.

**Zero-downtime CA migration.** Point `cafile` at a PEM **bundle** containing
both the old and new CA (the wrapper and `etcd_fdw_certificates` handle bundles),
reissue the etcd server certificate under the new CA, confirm all clients trust
both, then drop the old CA from the bundle.

Always rotate before `not_after` — monitor it as below.

### Monitoring certificate expiry

Three objects (extension 1.1+) report on a server's TLS material:

| Object | Returns |
|---|---|
| `etcd_fdw_certificates(server text)` | one row per cert: `source` (`peer`/`peer-chain`/`cafile`/`certfile`), `subject`, `issuer`, `not_before`, `not_after`, `expires_in` |
| `etcd_fdw_server_cert_expiry(server text)` | `timestamptz` — the etcd server (leaf) cert's `not_after` |
| `etcd_fdw_cert_expiry` (view) | the above across every `etcd_fdw` server, as `(server, source, subject, issuer, not_before, not_after, expires_in)` |

The `peer`/`peer-chain` rows are the certificate etcd actually presents (read via
the TLS handshake, regardless of trust, so an expired cert is still visible);
`cafile`/`certfile` rows are parsed from the local PEM files.

```sql
-- everything about one server's TLS
SELECT source, subject, not_after, expires_in FROM etcd_fdw_certificates('etcd');

-- alert: anything expiring within 30 days, across all etcd servers
SELECT server, source, subject, not_after
FROM etcd_fdw_cert_expiry
WHERE not_after < now() + interval '30 days'
ORDER BY not_after;
```

**Access control.** These functions expose only X.509 metadata (never private
keys) and are callable by a **superuser**, a member of **`pg_monitor`**, or a
role with **`USAGE` on the foreign server** (which includes the server's owner
and the owners/users of foreign tables on it). Other roles get
`permission denied`; the `etcd_fdw_cert_expiry` view simply omits servers the
caller may not inspect.

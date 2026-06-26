#!/usr/bin/env bash
#
# Validate etcd RBAC authentication and TLS end to end.  These need specially
# configured etcd instances, so they live outside the pg_regress matrix.
# Asserts on query results (deterministic), not on version-specific error text.
#
# Usage: run-auth-tls.sh [pg_major] [etcd_tag]
#
set -uo pipefail

PG_MAJOR="${1:-16}"
ETCD_TAG="${2:-v3.6.1}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ETCD_IMAGE="${ETCD_IMAGE:-quay.io/coreos/etcd:${ETCD_TAG}}"

SUFFIX="authtls_${PG_MAJOR}_$$"
NET="etcdfdw_${SUFFIX}"
PG="pg_${SUFFIX}"
EA="etcd_auth_${SUFFIX}"
ET="etcd_tls_${SUFFIX}"
# host dir for TLS certs, mounted into both the pg and etcd_tls containers
CERTS="/tmp/etcdfdw_certs_${SUFFIX}"

fail=0
cleanup() {
  docker rm -f "$PG" "$EA" "$ET" >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
  rm -rf "$CERTS"
}
trap cleanup EXIT

assert() { # <label> <expected> <actual>
  if [ "$2" = "$3" ]; then
    echo "PASS  $1"
  else
    echo "FAIL  $1: expected [$2] got [$3]"
    fail=1
  fi
}

docker network create "$NET" >/dev/null
mkdir -p "$CERTS"

# --- build the extension ---
docker run -d --name "$PG" --network "$NET" -e POSTGRES_PASSWORD=postgres \
  -v "$REPO_ROOT":/src -v "$CERTS":/certs "postgres:${PG_MAJOR}" >/dev/null
for i in $(seq 1 60); do docker exec "$PG" pg_isready -U postgres >/dev/null 2>&1 && break; sleep 1; done
docker exec "$PG" bash -lc '
  set -e; export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null
  apt-get install -y -qq build-essential postgresql-server-dev-'"$PG_MAJOR"' libcurl4-openssl-dev libssl-dev openssl >/dev/null
  rm -rf /build && cp -a /src /build
  cd /build && make clean >/dev/null 2>&1 || true; make >/dev/null && make install >/dev/null'
docker exec -u postgres "$PG" psql -q -c "CREATE EXTENSION etcd_fdw;" >/dev/null

# --- AUTH scenario ---
docker run -d --name "$EA" --network "$NET" --network-alias etcd_auth "$ETCD_IMAGE" \
  /usr/local/bin/etcd --name a1 \
  --advertise-client-urls http://0.0.0.0:2379 --listen-client-urls http://0.0.0.0:2379 >/dev/null
bash "$REPO_ROOT/test/scripts/wait-for-etcd.sh" "$EA" 60
docker exec "$EA" etcdctl user add root --new-user-password=rootpw >/dev/null
docker exec "$EA" etcdctl user grant-role root root >/dev/null 2>&1 || true
docker exec "$EA" etcdctl auth enable >/dev/null
docker exec "$EA" etcdctl --user root:rootpw put /auth/a1 1 >/dev/null
docker exec "$EA" etcdctl --user root:rootpw put /auth/a2 2 >/dev/null

AUTH_OUT=$(docker exec -i -u postgres "$PG" psql -qtA -d postgres <<'SQL'
CREATE SERVER s_auth FOREIGN DATA WRAPPER etcd_fdw OPTIONS (endpoints 'http://etcd_auth:2379');
CREATE USER MAPPING FOR postgres SERVER s_auth OPTIONS (username 'root', password 'rootpw');
CREATE FOREIGN TABLE akv (key text COLLATE "C", value text) SERVER s_auth OPTIONS (prefix '/auth/', strip_prefix 'true');
INSERT INTO akv (key, value) VALUES ('a3', '3');
SELECT string_agg(key || '=' || value, ',' ORDER BY key) FROM akv;
SQL
)
assert "auth read+write" "a1=1,a2=2,a3=3" "$AUTH_OUT"

# auth must fail over: a dead first endpoint must not make the cluster
# unreachable -- the token is fetched from the live node.
AUTH_FO=$(docker exec -i -u postgres "$PG" psql -qtA -d postgres <<'SQL'
CREATE SERVER s_auth_ha FOREIGN DATA WRAPPER etcd_fdw
  OPTIONS (endpoints 'http://dead.invalid:2379,http://etcd_auth:2379', connect_timeout_ms '1500');
CREATE USER MAPPING FOR postgres SERVER s_auth_ha OPTIONS (username 'root', password 'rootpw');
CREATE FOREIGN TABLE akv2 (key text COLLATE "C", value text) SERVER s_auth_ha OPTIONS (prefix '/auth/', strip_prefix 'true');
SELECT string_agg(key || '=' || value, ',' ORDER BY key) FROM akv2;
SQL
)
assert "auth fails over past a dead node" "a1=1,a2=2,a3=3" "$AUTH_FO"

# --- TLS scenario ---
docker exec "$PG" bash -lc '
  set -e; cd /certs
  cat > san.cnf <<EOF
[req]
distinguished_name = dn
req_extensions = v3_req
prompt = no
[dn]
CN = etcd_tls
[v3_req]
subjectAltName = DNS:etcd_tls,DNS:localhost,IP:127.0.0.1
EOF
  openssl genrsa -out ca.key 2048 2>/dev/null
  openssl req -x509 -new -nodes -key ca.key -subj "/CN=etcd-test-ca" -days 3650 -out ca.crt 2>/dev/null
  openssl genrsa -out server.key 2048 2>/dev/null
  openssl req -new -key server.key -subj "/CN=etcd_tls" -out server.csr -config san.cnf 2>/dev/null
  openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 3650 -out server.crt -extensions v3_req -extfile san.cnf 2>/dev/null
  chmod 644 *.crt *.key'

docker run -d --name "$ET" --network "$NET" --network-alias etcd_tls \
  -v "$CERTS":/certs:ro "$ETCD_IMAGE" \
  /usr/local/bin/etcd --name t1 \
  --advertise-client-urls https://0.0.0.0:2379 --listen-client-urls https://0.0.0.0:2379 \
  --cert-file=/certs/server.crt --key-file=/certs/server.key >/dev/null
sleep 3
docker exec "$ET" etcdctl --endpoints=https://127.0.0.1:2379 --cacert=/certs/ca.crt put /tls/t1 s1 >/dev/null
docker exec "$ET" etcdctl --endpoints=https://127.0.0.1:2379 --cacert=/certs/ca.crt put /tls/t2 s2 >/dev/null

TLS_OUT=$(docker exec -i -u postgres "$PG" psql -qtA -d postgres <<'SQL'
CREATE SERVER s_tls FOREIGN DATA WRAPPER etcd_fdw
  OPTIONS (endpoints 'https://etcd_tls:2379', use_tls 'true', cafile '/certs/ca.crt');
CREATE FOREIGN TABLE tkv (key text COLLATE "C", value text) SERVER s_tls OPTIONS (prefix '/tls/', strip_prefix 'true');
INSERT INTO tkv (key, value) VALUES ('t3', 's3');
SELECT string_agg(key || '=' || value, ',' ORDER BY key) FROM tkv;
SQL
)
assert "tls read+write" "t1=s1,t2=s2,t3=s3" "$TLS_OUT"

# --- certificate inspection (peer via libcurl, cafile via OpenSSL) ---
CERT_SRC=$(docker exec -i -u postgres "$PG" psql -qtA -d postgres -c \
  "SELECT string_agg(source, ',' ORDER BY source) FROM etcd_fdw_certificates('s_tls') WHERE not_after > now() + interval '3000 days';")
assert "cert sources (long-lived)" "cafile,peer" "$CERT_SRC"

PEER_CN=$(docker exec -i -u postgres "$PG" psql -qtA -d postgres -c \
  "SELECT subject LIKE '%etcd_tls%' FROM etcd_fdw_certificates('s_tls') WHERE source='peer';")
assert "peer subject is etcd_tls" "t" "$PEER_CN"

PEER_FUT=$(docker exec -i -u postgres "$PG" psql -qtA -d postgres -c \
  "SELECT etcd_fdw_server_cert_expiry('s_tls') > now();")
assert "peer expiry in the future" "t" "$PEER_FUT"

VIEW_OK=$(docker exec -i -u postgres "$PG" psql -qtA -d postgres -c \
  "SELECT count(*) FROM etcd_fdw_cert_expiry WHERE server='s_tls';")
assert "monitoring view rows" "2" "$VIEW_OK"

# privilege guard: an unprivileged role is denied; the view filters to empty
DENIED=$(docker exec -i -u postgres "$PG" psql -qtA -d postgres 2>&1 <<'X'
CREATE ROLE t_unpriv LOGIN;
SET ROLE t_unpriv;
SELECT count(*) FROM etcd_fdw_certificates('s_tls');
X
)
case "$DENIED" in
  *"permission denied"*) echo "PASS  privilege guard denies unprivileged role" ;;
  *) echo "FAIL  privilege guard: [$DENIED]"; fail=1 ;;
esac

echo "============================================================"
[ "$fail" -eq 0 ] && echo "auth/tls: all checks passed" || echo "auth/tls: FAILURES"
exit "$fail"

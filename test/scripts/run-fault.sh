#!/usr/bin/env bash
#
# Fault injection: an endpoint that always answers HTTP 503 (etcd "unavailable",
# as during a leader election).  Verifies the wrapper fails over from a 5xx node
# to a healthy one, and that an all-5xx cluster errors cleanly after its retries
# rather than returning the 503 or hanging.
#
# Usage: run-fault.sh [pg_major] [etcd_tag]
#
set -uo pipefail

PG_MAJOR="${1:-16}"
ETCD_TAG="${2:-v3.6.1}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ETCD_IMAGE="${ETCD_IMAGE:-quay.io/coreos/etcd:${ETCD_TAG}}"

SUFFIX="fault_${PG_MAJOR}_$$"
NET="etcdfdw_${SUFFIX}"
PG="pg_${SUFFIX}"
ETCD="etcd_real_${SUFFIX}"
BAD="etcd_bad_${SUFFIX}"
CONF="/tmp/fault_${SUFFIX}.conf"
fail=0

cleanup() {
  docker rm -f "$PG" "$ETCD" "$BAD" >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
  rm -f "$CONF"
}
trap cleanup EXIT

q() { docker exec -i "$PG" psql -U postgres -tA -d postgres 2>&1; }
assert() { if [ "$2" = "$3" ]; then echo "PASS  $1"; else echo "FAIL  $1: expected [$2] got [$3]"; fail=1; fi; }

docker network create "$NET" >/dev/null

# real etcd
docker run -d --name "$ETCD" --network "$NET" --network-alias etcd_real "$ETCD_IMAGE" \
  /usr/local/bin/etcd --name n1 \
  --advertise-client-urls http://0.0.0.0:2379 --listen-client-urls http://0.0.0.0:2379 >/dev/null
bash "$REPO_ROOT/test/scripts/wait-for-etcd.sh" "$ETCD" 60
docker exec "$ETCD" etcdctl put /f/k1 v1 >/dev/null
docker exec "$ETCD" etcdctl put /f/k2 v2 >/dev/null

# fault node: nginx that returns 503 for every request, on port 2379
cat > "$CONF" <<'EOF'
events {}
http {
  server {
    listen 2379;
    location / { return 503 "service unavailable"; }
  }
}
EOF
docker run -d --name "$BAD" --network "$NET" --network-alias etcd_bad \
  -v "$CONF":/etc/nginx/nginx.conf:ro nginx:alpine >/dev/null
sleep 2

# postgres + build
docker run -d --name "$PG" --network "$NET" -e POSTGRES_PASSWORD=postgres \
  -v "$REPO_ROOT":/src "postgres:${PG_MAJOR}" >/dev/null
for i in $(seq 1 60); do docker exec "$PG" pg_isready -U postgres >/dev/null 2>&1 && break; sleep 1; done
docker exec "$PG" bash -lc '
  set -e; export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null
  apt-get install -y -qq build-essential postgresql-server-dev-'"$PG_MAJOR"' libcurl4-openssl-dev libssl-dev >/dev/null
  rm -rf /build && cp -a /src /build && cd /build && make >/dev/null && make install >/dev/null'
docker exec -i "$PG" psql -U postgres -q <<'SQL'
CREATE EXTENSION etcd_fdw;
SQL

# Test 1: bad (503) first, healthy second -> fail over on 5xx
docker exec -i "$PG" psql -U postgres -q <<'SQL'
CREATE SERVER s1 FOREIGN DATA WRAPPER etcd_fdw
  OPTIONS (endpoints 'http://etcd_bad:2379,http://etcd_real:2379',
           connect_timeout_ms '1500', max_retries '2', retry_backoff_ms '100');
CREATE FOREIGN TABLE t1 (key text COLLATE "C", value text) SERVER s1 OPTIONS (prefix '/f/', strip_prefix 'true');
SQL
OUT=$(echo "SELECT count(*) FROM t1;" | q)
assert "fail over from 503 node to healthy node" "2" "$OUT"

# Test 2: only the 503 node -> clean error after retries (not a returned 503)
docker exec -i "$PG" psql -U postgres -q <<'SQL'
CREATE SERVER s2 FOREIGN DATA WRAPPER etcd_fdw
  OPTIONS (endpoints 'http://etcd_bad:2379', max_retries '1', retry_backoff_ms '50');
CREATE FOREIGN TABLE t2 (key text COLLATE "C", value text) SERVER s2 OPTIONS (prefix '/f/', strip_prefix 'true');
SQL
OUT=$(echo "SELECT count(*) FROM t2;" | q)
case "$OUT" in
  *"no etcd endpoint available"*) echo "PASS  all-5xx errors cleanly after retries" ;;
  *) echo "FAIL  all-5xx error: [$OUT]"; fail=1 ;;
esac

echo "============================================================"
[ "$fail" -eq 0 ] && echo "fault: all checks passed" || echo "fault: FAILURES"
exit "$fail"

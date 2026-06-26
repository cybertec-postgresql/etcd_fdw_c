#!/usr/bin/env bash
#
# High-availability validation: a server configured with multiple endpoints
# keeps working when a node fails, and errors cleanly only when all are down.
#
# Usage: run-ha.sh [pg_major] [etcd_tag]
#
set -uo pipefail

PG_MAJOR="${1:-16}"
ETCD_TAG="${2:-v3.6.1}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ETCD_IMAGE="${ETCD_IMAGE:-quay.io/coreos/etcd:${ETCD_TAG}}"

SUFFIX="ha_${PG_MAJOR}_$$"
NET="etcdfdw_${SUFFIX}"
PG="pg_${SUFFIX}"
A="etcd_a_${SUFFIX}"
B="etcd_b_${SUFFIX}"
fail=0

cleanup() {
  docker rm -f "$PG" "$A" "$B" >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
}
trap cleanup EXIT

q() { docker exec -i "$PG" psql -U postgres -tA -d postgres 2>&1; }
assert() { if [ "$2" = "$3" ]; then echo "PASS  $1"; else echo "FAIL  $1: expected [$2] got [$3]"; fail=1; fi; }

docker network create "$NET" >/dev/null
for node in "$A" "$B"; do
  docker run -d --name "$node" --network "$NET" --network-alias "$node" "$ETCD_IMAGE" \
    /usr/local/bin/etcd --name "$node" \
    --advertise-client-urls http://0.0.0.0:2379 --listen-client-urls http://0.0.0.0:2379 >/dev/null
  bash "$REPO_ROOT/test/scripts/wait-for-etcd.sh" "$node" 60
  docker exec "$node" etcdctl put /ha/k1 v1 >/dev/null
  docker exec "$node" etcdctl put /ha/k2 v2 >/dev/null
done

docker run -d --name "$PG" --network "$NET" -e POSTGRES_PASSWORD=postgres \
  -v "$REPO_ROOT":/src "postgres:${PG_MAJOR}" >/dev/null
for i in $(seq 1 60); do docker exec "$PG" pg_isready -U postgres >/dev/null 2>&1 && break; sleep 1; done
docker exec "$PG" bash -lc '
  set -e; export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null
  apt-get install -y -qq build-essential postgresql-server-dev-'"$PG_MAJOR"' libcurl4-openssl-dev libssl-dev >/dev/null
  rm -rf /build && cp -a /src /build && cd /build && make >/dev/null && make install >/dev/null'

docker exec -i "$PG" psql -U postgres -q -d postgres >/dev/null 2>&1 <<SQL
CREATE EXTENSION etcd_fdw;
CREATE SERVER ha FOREIGN DATA WRAPPER etcd_fdw
  OPTIONS (endpoints 'http://$A:2379,http://$B:2379',
           connect_timeout_ms '1500', max_retries '3', retry_backoff_ms '150');
CREATE FOREIGN TABLE hkv (key text COLLATE "C", value text) SERVER ha OPTIONS (prefix '/ha/', strip_prefix 'true');
SQL

OUT=$(echo "SELECT count(*) FROM hkv;" | q)
assert "both nodes up" "2" "$OUT"

# ALTER SERVER mid-session must invalidate the cached connection and rebuild it
# cleanly on the next query in the SAME backend (no crash / use-after-free).
OUT=$(docker exec -i "$PG" psql -U postgres -qtA -d postgres \
        -c "ALTER SERVER ha OPTIONS (SET connect_timeout_ms '2000')" \
        -c "SELECT count(*) FROM hkv" 2>&1)
assert "reconnect after ALTER SERVER mid-session" "2" "$OUT"

# kill the node the connection is currently pinned to (the first endpoint)
docker stop "$A" >/dev/null
OUT=$(echo "SELECT count(*) FROM hkv;" | q)
assert "failover after primary node down" "2" "$OUT"

# all nodes down -> clean error, not a hang
docker stop "$B" >/dev/null
OUT=$(echo "SELECT count(*) FROM hkv;" | q)
case "$OUT" in
  *"no etcd endpoint available"*) echo "PASS  clean error when all nodes down" ;;
  *) echo "FAIL  all-down error: [$OUT]"; fail=1 ;;
esac

# recovery: bring a node back -> queries succeed again
docker start "$B" >/dev/null
bash "$REPO_ROOT/test/scripts/wait-for-etcd.sh" "$B" 60
OUT=$(echo "SELECT count(*) FROM hkv;" | q)
assert "recovery after node returns" "2" "$OUT"

echo "============================================================"
[ "$fail" -eq 0 ] && echo "ha: all checks passed" || echo "ha: FAILURES"
exit "$fail"

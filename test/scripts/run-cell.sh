#!/usr/bin/env bash
#
# Run one test cell: build etcd_fdw against a PostgreSQL major version and run
# the pg_regress suite against a specific etcd version, all in throwaway Docker
# containers.
#
# Usage: run-cell.sh <pg_major> <etcd_tag>
#   e.g. run-cell.sh 16 v3.5.16
#
set -euo pipefail

PG_MAJOR="${1:?pg major required, e.g. 16}"
ETCD_TAG="${2:?etcd tag required, e.g. v3.5.16}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ETCD_IMAGE="${ETCD_IMAGE:-quay.io/coreos/etcd:${ETCD_TAG}}"
PG_IMAGE="postgres:${PG_MAJOR}"

SUFFIX="${PG_MAJOR}_${ETCD_TAG//./_}_$$"
NET="etcdfdw_${SUFFIX}"
ETCD="etcd"                       # tests connect to http://etcd:2379
PG="pg_${SUFFIX}"

cleanup() {
  docker rm -f "$PG" "$ETCD.$SUFFIX" >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "=== cell: PostgreSQL ${PG_MAJOR} x etcd ${ETCD_TAG} ==="
docker network create "$NET" >/dev/null

# etcd (network alias 'etcd' so the regress SQL can hardcode http://etcd:2379)
docker run -d --name "$ETCD.$SUFFIX" --network "$NET" --network-alias "$ETCD" \
  "$ETCD_IMAGE" \
  /usr/local/bin/etcd --name n1 \
  --advertise-client-urls http://0.0.0.0:2379 \
  --listen-client-urls http://0.0.0.0:2379 >/dev/null
bash "$REPO_ROOT/test/scripts/wait-for-etcd.sh" "$ETCD.$SUFFIX" 60

# postgres
docker run -d --name "$PG" --network "$NET" \
  -e POSTGRES_PASSWORD=postgres \
  -v "$REPO_ROOT":/src "$PG_IMAGE" >/dev/null
for i in $(seq 1 60); do
  docker exec "$PG" pg_isready -U postgres >/dev/null 2>&1 && break
  sleep 1
done

# build + install + run the regression suite.
# Build in an isolated copy (/build) so concurrent runs never share .o files
# and the mounted source tree stays clean.
docker exec "$PG" bash -lc '
  set -e
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null
  apt-get install -y -qq build-essential postgresql-server-dev-'"$PG_MAJOR"' \
        libcurl4-openssl-dev libssl-dev >/dev/null
  rm -rf /build && cp -a /src /build
  cd /build
  make clean >/dev/null 2>&1 || true
  make >/dev/null
  make install >/dev/null
  chown -R postgres /build
'
docker exec -u postgres "$PG" bash -lc 'cd /build && make installcheck'
status=$?

if [ "$status" -ne 0 ]; then
  echo "--- regression.diffs ---"
  docker exec "$PG" bash -lc 'cat /build/test/regression.diffs 2>/dev/null || true'
fi
exit "$status"

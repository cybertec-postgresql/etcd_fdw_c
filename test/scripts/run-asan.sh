#!/usr/bin/env bash
#
# Build etcd_fdw with AddressSanitizer and run the full regression suite under
# it, failing if the sanitizer reports any memory error.
#
# Usage: run-asan.sh [pg_major] [etcd_tag]
#
set -uo pipefail

PG_MAJOR="${1:-16}"
ETCD_TAG="${2:-v3.6.1}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ETCD_IMAGE="${ETCD_IMAGE:-quay.io/coreos/etcd:${ETCD_TAG}}"

SUFFIX="asan_${PG_MAJOR}_$$"
NET="etcdfdw_${SUFFIX}"
ETCD="etcd_${SUFFIX}"
PG="pg_${SUFFIX}"

cleanup() {
  docker rm -f "$PG" "$ETCD" >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "=== ASan: PostgreSQL ${PG_MAJOR} x etcd ${ETCD_TAG} ==="
docker network create "$NET" >/dev/null
docker run -d --name "$ETCD" --network "$NET" --network-alias etcd "$ETCD_IMAGE" \
  /usr/local/bin/etcd --name n1 \
  --advertise-client-urls http://0.0.0.0:2379 \
  --listen-client-urls http://0.0.0.0:2379 >/dev/null
bash "$REPO_ROOT/test/scripts/wait-for-etcd.sh" "$ETCD" 60

# keep the container alive without the auto-started server
docker run -d --name "$PG" --network "$NET" -v "$REPO_ROOT":/src \
  --entrypoint bash postgres:"$PG_MAJOR" -c 'sleep infinity' >/dev/null

docker exec "$PG" bash -lc '
  set -e
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null
  apt-get install -y -qq build-essential postgresql-server-dev-'"$PG_MAJOR"' \
        libcurl4-openssl-dev libssl-dev >/dev/null
  rm -rf /build && cp -a /src /build
  cd /build && make clean >/dev/null 2>&1 || true
  make COPT="-fsanitize=address -fno-omit-frame-pointer -O1 -g" \
       SHLIB_LINK="-lcurl -fsanitize=address" >/dev/null
  make install >/dev/null
  chown -R postgres /build /var/lib/postgresql
'

# init + start server under ASan, run the suite, then look for sanitizer reports
docker exec "$PG" bash -lc '
  set -e
  BIN=$(pg_config --bindir)
  LIBASAN=$(gcc -print-file-name=libasan.so)
  rm -rf /tmp/data /tmp/asan.*
  su postgres -c "$BIN/initdb -D /tmp/data" >/dev/null 2>&1
  su postgres -c "ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:log_path=/tmp/asan \
      LD_PRELOAD=$LIBASAN $BIN/postgres -D /tmp/data -k /tmp -p 5432" \
      >/tmp/pg.log 2>&1 &
  for i in $(seq 1 30); do
    su postgres -c "$BIN/pg_isready -h /tmp -p 5432" >/dev/null 2>&1 && break
    sleep 1
  done
  su postgres -c "cd /build && PGHOST=/tmp PGPORT=5432 make installcheck"
'
status=$?

echo "--- scanning for AddressSanitizer reports ---"
if docker exec "$PG" bash -lc 'ls /tmp/asan.* >/dev/null 2>&1'; then
  docker exec "$PG" bash -lc 'cat /tmp/asan.*'
  echo "ASAN REPORTED ERRORS"
  exit 1
fi

if [ "$status" -ne 0 ]; then
  docker exec "$PG" bash -lc 'cat /build/test/regression.diffs 2>/dev/null || true'
  exit "$status"
fi
echo "ASan clean, all tests passed"

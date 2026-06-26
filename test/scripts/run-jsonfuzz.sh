#!/usr/bin/env bash
#
# JSON fuzzing: point the wrapper at a mock etcd that returns malformed /
# adversarial response bodies, with the extension built under AddressSanitizer.
# Every query must error cleanly; the backend must never crash and ASan must
# stay silent.
#
# Usage: run-jsonfuzz.sh [pg_major] [iterations]
#
set -uo pipefail

PG_MAJOR="${1:-16}"
ITERS="${2:-300}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

SUFFIX="jsonfuzz_${PG_MAJOR}_$$"
NET="etcdfdw_${SUFFIX}"
PG="pg_${SUFFIX}"
MOCK="mock_${SUFFIX}"
fail=0

cleanup() {
  docker rm -f "$PG" "$MOCK" >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker network create "$NET" >/dev/null

# mock etcd returning broken JSON
docker run -d --name "$MOCK" --network "$NET" --network-alias mock \
  -v "$REPO_ROOT/test/docker/mock_etcd.py":/mock_etcd.py:ro \
  python:3-alpine python3 /mock_etcd.py >/dev/null
sleep 2

# postgres container (kept alive; server started by hand under ASan)
docker run -d --name "$PG" --network "$NET" -v "$REPO_ROOT":/src \
  --entrypoint bash "postgres:${PG_MAJOR}" -c 'sleep infinity' >/dev/null
docker exec "$PG" bash -lc '
  set -e; export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null
  apt-get install -y -qq build-essential postgresql-server-dev-'"$PG_MAJOR"' libcurl4-openssl-dev libssl-dev >/dev/null
  rm -rf /build && cp -a /src /build && cd /build
  make COPT="-fsanitize=address -fno-omit-frame-pointer -O1 -g" \
       SHLIB_LINK="-lcurl -fsanitize=address" >/dev/null
  make install >/dev/null
  chown -R postgres /build /var/lib/postgresql'

# build the fuzz script: setup, then many queries against the mock (errors
# ignored), then a liveness check that the backend is still alive.
{
  echo "CREATE EXTENSION etcd_fdw;"
  echo "CREATE SERVER m FOREIGN DATA WRAPPER etcd_fdw OPTIONS (endpoints 'http://mock:2379', max_retries '0');"
  echo "CREATE FOREIGN TABLE fz (key text COLLATE \"C\", value text, mod_revision bigint) SERVER m OPTIONS (prefix '/z/', strip_prefix 'true');"
  echo "\\set ON_ERROR_STOP off"
  for i in $(seq 1 "$ITERS"); do
    case $((i % 5)) in
      0) echo "SELECT count(*) FROM fz;" ;;
      1) echo "SELECT key, value FROM fz WHERE key = 'k$i';" ;;
      2) echo "SELECT key FROM fz WHERE key LIKE 'a%' ORDER BY key;" ;;
      3) echo "SELECT key FROM fz WHERE key IN ('a','b','c');" ;;
      4) echo "INSERT INTO fz (key, value) VALUES ('k$i', 'v$i');" ;;
    esac
  done
  echo "\\echo FUZZ_DONE_LIVENESS"
  echo "SELECT 1 AS alive;"
} > /tmp/fuzz_${SUFFIX}.sql
docker cp /tmp/fuzz_${SUFFIX}.sql "$PG":/tmp/fuzz.sql
rm -f /tmp/fuzz_${SUFFIX}.sql

docker exec "$PG" bash -lc '
  set -e
  BIN=$(pg_config --bindir); LIBASAN=$(gcc -print-file-name=libasan.so)
  rm -rf /tmp/data /tmp/asan.*
  su postgres -c "$BIN/initdb -D /tmp/data" >/dev/null 2>&1
  su postgres -c "ASAN_OPTIONS=detect_leaks=0:abort_on_error=0:log_path=/tmp/asan \
      LD_PRELOAD=$LIBASAN $BIN/postgres --single -D /tmp/data postgres" \
      < /tmp/fuzz.sql > /tmp/fuzz.out 2>&1 || true'

# liveness: the final SELECT 1 must have executed (backend survived the fuzz)
if docker exec "$PG" bash -lc 'grep -q "alive" /tmp/fuzz.out'; then
  echo "PASS  backend survived ${ITERS} malformed responses"
else
  echo "FAIL  backend did not reach the liveness check"
  docker exec "$PG" bash -lc 'tail -20 /tmp/fuzz.out' || true
  fail=1
fi

# AddressSanitizer must be silent
if docker exec "$PG" bash -lc 'ls /tmp/asan.* >/dev/null 2>&1'; then
  echo "FAIL  AddressSanitizer reported errors"
  docker exec "$PG" bash -lc 'cat /tmp/asan.*' || true
  fail=1
else
  echo "PASS  AddressSanitizer clean under fuzzing"
fi

echo "============================================================"
[ "$fail" -eq 0 ] && echo "jsonfuzz: all checks passed" || echo "jsonfuzz: FAILURES"
exit "$fail"

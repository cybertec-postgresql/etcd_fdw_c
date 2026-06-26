#!/usr/bin/env bash
#
# Concurrency validation: many PostgreSQL backends hammering etcd at once.
#
#  1. Lost-update test: N workers each do M guarded increments of one counter,
#     retrying on serialization_failure.  The optimistic mod_revision guard must
#     yield an exact final total (no lost updates).
#  2. Parallel disjoint inserts: the total row count must be exact.
#  3. No backend may crash.
#
# Usage: run-concurrency.sh [pg_major] [etcd_tag] [workers] [iters]
#
set -uo pipefail

PG_MAJOR="${1:-16}"
ETCD_TAG="${2:-v3.6.1}"
WORKERS="${3:-8}"
ITERS="${4:-50}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ETCD_IMAGE="${ETCD_IMAGE:-quay.io/coreos/etcd:${ETCD_TAG}}"

SUFFIX="conc_${PG_MAJOR}_$$"
NET="etcdfdw_${SUFFIX}"
PG="pg_${SUFFIX}"
ETCD="etcd_${SUFFIX}"
fail=0

cleanup() {
  docker rm -f "$PG" "$ETCD" >/dev/null 2>&1 || true
  docker network rm "$NET" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker network create "$NET" >/dev/null
docker run -d --name "$ETCD" --network "$NET" --network-alias etcd "$ETCD_IMAGE" \
  /usr/local/bin/etcd --name n1 \
  --advertise-client-urls http://0.0.0.0:2379 --listen-client-urls http://0.0.0.0:2379 >/dev/null
bash "$REPO_ROOT/test/scripts/wait-for-etcd.sh" "$ETCD" 60

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
CREATE SERVER etcd FOREIGN DATA WRAPPER etcd_fdw OPTIONS (endpoints 'http://etcd:2379');
CREATE FOREIGN TABLE ctr (key text COLLATE "C", value text, mod_revision bigint)
  SERVER etcd OPTIONS (prefix '/conc/', strip_prefix 'true');
INSERT INTO ctr (key, value) VALUES ('counter', '0');
CREATE FOREIGN TABLE st (key text COLLATE "C", value text)
  SERVER etcd OPTIONS (prefix '/stress/', strip_prefix 'true');
SQL

# worker DO block: ITERS guarded increments with retry
cat > /tmp/conc_worker_${SUFFIX}.sql <<SQL
DO \$\$
DECLARE i int;
BEGIN
  FOR i IN 1..${ITERS} LOOP
    LOOP
      BEGIN
        UPDATE ctr SET value = ((value::int) + 1)::text WHERE key = 'counter';
        EXIT;
      EXCEPTION WHEN serialization_failure THEN
        -- retry
      END;
    END LOOP;
  END LOOP;
END\$\$;
SQL
docker cp /tmp/conc_worker_${SUFFIX}.sql "$PG":/tmp/worker.sql
rm -f /tmp/conc_worker_${SUFFIX}.sql

echo "=== lost-update test: ${WORKERS} workers x ${ITERS} guarded increments ==="
pids=""
for w in $(seq 1 "$WORKERS"); do
  docker exec -i "$PG" psql -U postgres -q -f /tmp/worker.sql >/dev/null 2>&1 &
  pids="$pids $!"
done
wait $pids
expected=$((WORKERS * ITERS))
got=$(docker exec -i "$PG" psql -U postgres -tAc "SELECT value FROM ctr WHERE key='counter';")
if [ "$got" = "$expected" ]; then echo "PASS  no lost updates ($got)"; else echo "FAIL  counter=$got expected=$expected"; fail=1; fi

echo "=== parallel disjoint inserts: ${WORKERS} x 100 ==="
pids=""
for w in $(seq 1 "$WORKERS"); do
  docker exec -i "$PG" psql -U postgres -q -c \
    "INSERT INTO st(key,value) SELECT 'w${w}_'||lpad(g::text,4,'0'),'v'||g FROM generate_series(1,100) g;" \
    >/dev/null 2>&1 &
  pids="$pids $!"
done
wait $pids
exp2=$((WORKERS * 100))
got2=$(docker exec -i "$PG" psql -U postgres -tAc "SELECT count(*) FROM st;")
if [ "$got2" = "$exp2" ]; then echo "PASS  insert count ($got2)"; else echo "FAIL  count=$got2 expected=$exp2"; fail=1; fi

if docker logs "$PG" 2>&1 | grep -qiE "terminated by signal|was terminated|segmentation"; then
  echo "FAIL  a backend crashed"; fail=1
else
  echo "PASS  no backend crashes"
fi

echo "============================================================"
[ "$fail" -eq 0 ] && echo "concurrency: all checks passed" || echo "concurrency: FAILURES"
exit "$fail"

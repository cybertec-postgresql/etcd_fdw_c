#!/usr/bin/env bash
#
# Run the wrapper's hot paths under Valgrind memcheck (complements ASan).
# PostgreSQL itself is not Valgrind-clean unless specially built, so we only
# fail on memcheck errors whose stack involves *our* code (src/etcd_*,
# deparse.c, options.c) — the extension, not the server internals.
#
# Usage: run-valgrind.sh [pg_major] [etcd_tag]
#
set -uo pipefail

PG_MAJOR="${1:-16}"
ETCD_TAG="${2:-v3.6.1}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ETCD_IMAGE="${ETCD_IMAGE:-quay.io/coreos/etcd:${ETCD_TAG}}"

SUFFIX="vg_${PG_MAJOR}_$$"
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
docker exec "$ETCD" etcdctl put /vg/svc1/a 1 >/dev/null   # for IMPORT

docker run -d --name "$PG" --network "$NET" -v "$REPO_ROOT":/src \
  --entrypoint bash "postgres:${PG_MAJOR}" -c 'sleep infinity' >/dev/null
docker exec "$PG" bash -lc '
  set -e; export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq >/dev/null
  apt-get install -y -qq build-essential postgresql-server-dev-'"$PG_MAJOR"' \
        libcurl4-openssl-dev libssl-dev valgrind >/dev/null
  rm -rf /build && cp -a /src /build && cd /build
  make COPT="-O0 -g -fno-omit-frame-pointer" >/dev/null
  make install >/dev/null
  chown -R postgres /build /var/lib/postgresql'

# exercise as many code paths as possible in one single-user session
cat > /tmp/vg_${SUFFIX}.sql <<'SQL'
CREATE EXTENSION etcd_fdw;
CREATE SERVER etcd FOREIGN DATA WRAPPER etcd_fdw OPTIONS (endpoints 'http://etcd:2379', use_remote_estimate 'true');
CREATE FOREIGN TABLE kv (key text COLLATE "C", value text, mod_revision bigint, version bigint, lease bigint)
  SERVER etcd OPTIONS (prefix '/vg/kv/', strip_prefix 'true');
DELETE FROM kv;
INSERT INTO kv (key, value) VALUES ('a','1'),('b','2'),('c','3');
INSERT INTO kv (key, value) SELECT 'm'||g, g::text FROM generate_series(1,60) g;
SELECT count(*) FROM kv;
SELECT key,value FROM kv WHERE key='a';
SELECT key FROM kv WHERE key >= 'a' AND key < 'c' ORDER BY key;
SELECT key FROM kv WHERE key LIKE 'm%' ORDER BY key DESC LIMIT 5;
SELECT key FROM kv WHERE key IN ('a','b','zz');
SELECT key FROM kv WHERE value = '2';
UPDATE kv SET value='9' WHERE key='a';
UPDATE kv SET key='renamed' WHERE key='b';
DELETE FROM kv WHERE key='c';
SELECT etcd_fdw_lease_grant('etcd', 60) AS lid \gset
SELECT etcd_fdw_lease_ttl('etcd', :lid);
INSERT INTO kv (key,value,lease) VALUES ('eph','x', :lid);
SELECT etcd_fdw_lease_keepalive('etcd', :lid);
SELECT etcd_fdw_lease_revoke('etcd', :lid);
CREATE SCHEMA vimp;
IMPORT FOREIGN SCHEMA "/vg/" FROM SERVER etcd INTO vimp;
ANALYZE kv;
TRUNCATE kv;
SQL
docker cp /tmp/vg_${SUFFIX}.sql "$PG":/tmp/vg.sql
rm -f /tmp/vg_${SUFFIX}.sql

docker exec "$PG" bash -lc '
  set -e
  BIN=$(pg_config --bindir)
  rm -rf /tmp/data; su postgres -c "$BIN/initdb -D /tmp/data" >/dev/null 2>&1
  su postgres -c "valgrind --leak-check=no --error-exitcode=0 --trace-children=no \
      $BIN/postgres --single -D /tmp/data postgres" < /tmp/vg.sql > /tmp/vg.log 2>&1 || true'

echo "--- Valgrind ERROR SUMMARY ---"
docker exec "$PG" bash -lc 'grep "ERROR SUMMARY" /tmp/vg.log | tail -1' || true

# keep only memcheck errors whose stack involves our extension
docker exec "$PG" bash -lc '
awk "
/^==[0-9]+== (Invalid|Mismatched|Source and destination overlap|Use of uninitialised|Conditional jump or move depends on uninitialised)/ {
  if (inblk && hit) print blk;
  inblk=1; hit=0; blk=\$0; next
}
inblk {
  blk=blk \"\n\" \$0
  if (\$0 ~ /etcd_client|etcd_conn|etcd_json|deparse|options\.c|etcd_cert|etcd_lease|etcd_fdw\.(c|so)/) hit=1
  if (\$0 ~ /^==[0-9]+== *\$/) { if (hit) print blk; inblk=0; hit=0; blk=\"\" }
}
END { if (inblk && hit) print blk }
" /tmp/vg.log' > /tmp/vg_ours_${SUFFIX}.txt 2>/dev/null

if [ -s /tmp/vg_ours_${SUFFIX}.txt ]; then
  echo "FAIL  Valgrind reported errors in etcd_fdw code:"
  sed -n '1,60p' /tmp/vg_ours_${SUFFIX}.txt
  fail=1
else
  echo "PASS  no Valgrind memcheck errors in etcd_fdw code"
fi
rm -f /tmp/vg_ours_${SUFFIX}.txt

echo "============================================================"
[ "$fail" -eq 0 ] && echo "valgrind: all checks passed" || echo "valgrind: FAILURES"
exit "$fail"

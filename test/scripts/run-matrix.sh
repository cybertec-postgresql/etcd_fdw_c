#!/usr/bin/env bash
#
# Run the full PostgreSQL x etcd test matrix in containers.
#
# Override the version lists via env, e.g.:
#   PG_VERSIONS="16 17" ETCD_TAGS="v3.6.1" ./run-matrix.sh
#
set -uo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

PG_VERSIONS="${PG_VERSIONS:-14 15 16 17 18}"
ETCD_TAGS="${ETCD_TAGS:-v3.4.34 v3.5.16 v3.6.1}"

pass=0
fail=0
failed_cells=""

for pg in $PG_VERSIONS; do
  for etcd in $ETCD_TAGS; do
    if bash "$REPO_ROOT/test/scripts/run-cell.sh" "$pg" "$etcd"; then
      echo "PASS  pg=$pg etcd=$etcd"
      pass=$((pass + 1))
    else
      echo "FAIL  pg=$pg etcd=$etcd"
      fail=$((fail + 1))
      failed_cells="${failed_cells} pg${pg}/${etcd}"
    fi
  done
done

echo "============================================================"
echo "matrix complete: ${pass} passed, ${fail} failed"
[ -n "$failed_cells" ] && echo "failed:${failed_cells}"
[ "$fail" -eq 0 ]

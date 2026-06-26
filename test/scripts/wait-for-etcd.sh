#!/usr/bin/env bash
# Wait until an etcd endpoint answers its health probe.
# Usage: wait-for-etcd.sh <container-name> [timeout-seconds]
set -euo pipefail
name="${1:?container name required}"
timeout="${2:-60}"

for i in $(seq 1 "$timeout"); do
  if docker exec "$name" etcdctl endpoint health >/dev/null 2>&1; then
    echo "etcd '$name' healthy after ${i}s"
    exit 0
  fi
  sleep 1
done
echo "etcd '$name' did not become healthy within ${timeout}s" >&2
docker logs "$name" 2>&1 | tail -20 >&2
exit 1

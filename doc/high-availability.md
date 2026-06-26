# High availability & resilience

`etcd_fdw` is built to keep working while individual etcd nodes come and go.

## Multiple endpoints

List every etcd client URL in the `endpoints` option (comma-separated). A single
endpoint is fine; a list gives failover:

```sql
CREATE SERVER etcd FOREIGN DATA WRAPPER etcd_fdw OPTIONS (
  endpoints 'https://etcd1:2379,https://etcd2:2379,https://etcd3:2379',
  use_tls   'true',
  cafile    '/etc/pki/etcd/ca.crt'
);
```

A bare `host:port` is accepted and assumed to be `http://`.

## How failover and retries work

For each request the wrapper walks the endpoint list starting from the
last-known-good node:

1. **Transport failure** (connection refused, DNS failure, timeout) → try the
   next endpoint.
2. **HTTP 5xx / 429** (etcd momentarily unavailable, e.g. during a leader
   election) → try the next endpoint.
3. **2xx / definitive 4xx** → return immediately.

If a whole pass over the list turns up nothing usable, the wrapper backs off and
repeats the pass up to `max_retries` times, so a brief outage (a few seconds of
leader election) is ridden out transparently. Only when every endpoint is down
across all attempts does a query fail, with:

```
ERROR:  etcd_fdw: no etcd endpoint available after N attempt(s)
DETAIL:  last error: ...
```

The last-good endpoint is remembered, so steady-state traffic sticks to one node
instead of re-probing dead ones.

### Tuning

| Option | Default | Purpose |
|---|---|---|
| `max_retries` | `2` | Extra full passes over the endpoint list (total attempts = `max_retries + 1`). |
| `retry_backoff_ms` | `200` | Base delay between passes (scaled by pass number). |
| `connect_timeout_ms` | `3000` | How long to wait per node before failing over. |
| `request_timeout_ms` | `30000` | Overall per-request cap. |

For latency-sensitive workloads, lower `connect_timeout_ms` so failover to a
healthy node happens quickly; raise `max_retries`/`retry_backoff_ms` if your
cluster has longer elections. Backoff waits honor query cancellation.

## Behaviour during common events

| Event | What happens |
|---|---|
| One node crashes | Next request fails over to a surviving node; recovers automatically. |
| Leader election (brief 5xx / "no leader") | Retried across nodes and passes; usually invisible to the query. |
| A node returns | Picked up automatically on the next request (the wrapper re-probes the list). |
| All nodes down | The query errors after the configured attempts (it does not hang). |
| Server endpoints changed (`ALTER SERVER`) | Cached connections are invalidated cluster-wide; new connections use the new list. |

## Reads, writes, and consistency

- **Reads are linearizable.** The wrapper never sets etcd's `serializable` flag,
  so a `Range` is a quorum read — it always reflects the latest committed data,
  never a stale follower. A single query is one atomic snapshot at one revision
  (no torn reads).
- **Writes are durable and synchronous.** `Put`/`DeleteRange`/`Txn` return only
  after etcd commits to a quorum; the call is synchronous (no client-side
  buffering). Retries are safe: `Put` is idempotent, and a guarded
  `UPDATE`/`DELETE` whose result was lost re-checks `mod_revision` on retry
  (a no-op second apply surfaces as a `40001` to retry, never a double apply).
- **Failures surface, they are never silent.** A truncated transfer becomes a
  transport error (retried/failed-over); an unparseable body becomes a clean
  `ERROR`. You never receive a short result that looks complete.

## Authentication failover

An etcd auth token is valid cluster-wide, so the wrapper obtains it from **any
reachable node**: if the node it would normally use is down, it tries the next
endpoint. A single dead node never makes an authenticated cluster unreachable.

## Notes

- Connections are cached per `(server, user)` per backend and reused with
  keep-alive; failover happens on the next request that hits a dead node.
- etcd writes are not transactional with PostgreSQL — see
  [transactions.md](transactions.md). Failover does not change that: a write that
  returned success has been applied; one that errored may need to be retried by
  the application.
- To force a backend to drop and rebuild its connections immediately (e.g. after
  maintenance), call `SELECT etcd_fdw_disconnect();`.

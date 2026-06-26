-- pushdown: key predicate / ORDER BY / LIMIT pushdown to etcd Range
CREATE FOREIGN TABLE p_kv (key text COLLATE "C", value text)
  SERVER etcd OPTIONS (prefix '/test/push/', strip_prefix 'true');

DELETE FROM p_kv;
INSERT INTO p_kv (key, value)
  SELECT chr(96 + g), 'v' || g FROM generate_series(1, 7) g;   -- a..g

-- equality -> single-key get
EXPLAIN (VERBOSE, COSTS OFF) SELECT key, value FROM p_kv WHERE key = 'c';
SELECT key, value FROM p_kv WHERE key = 'c';

-- range -> bounded Range
EXPLAIN (VERBOSE, COSTS OFF) SELECT key FROM p_kv WHERE key >= 'c' AND key < 'f' ORDER BY key;
SELECT key FROM p_kv WHERE key >= 'c' AND key < 'f' ORDER BY key;

-- prefix LIKE -> prefix Range
SELECT key FROM p_kv WHERE key LIKE 'a%';

-- IN-list -> multi-key fetch
EXPLAIN (VERBOSE, COSTS OFF) SELECT key, value FROM p_kv WHERE key IN ('b', 'd', 'zzz');
SELECT key, value FROM p_kv WHERE key IN ('b', 'd', 'zzz') ORDER BY key;

-- ORDER BY DESC + LIMIT pushed down
EXPLAIN (VERBOSE, COSTS OFF) SELECT key FROM p_kv ORDER BY key DESC LIMIT 3;
SELECT key FROM p_kv ORDER BY key DESC LIMIT 3;

-- filter on value stays a local recheck
EXPLAIN (VERBOSE, COSTS OFF) SELECT key FROM p_kv WHERE value = 'v2';
SELECT key FROM p_kv WHERE value = 'v2';

DELETE FROM p_kv;
DROP FOREIGN TABLE p_kv;

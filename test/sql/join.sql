-- join: parameterized foreign scan (key = outer column).
-- Validates correctness regardless of the plan shape chosen; the parameterized
-- path itself is exercised under a forced nested loop with remote estimates.
CREATE FOREIGN TABLE j_kv (key text COLLATE "C", value text)
  SERVER etcd OPTIONS (prefix '/test/join/', strip_prefix 'true', use_remote_estimate 'true');

DELETE FROM j_kv;
INSERT INTO j_kv (key, value) SELECT 'k' || lpad(g::text, 4, '0'), 'v' || g
  FROM generate_series(1, 200) g;

CREATE TEMP TABLE probe (k text COLLATE "C");
INSERT INTO probe VALUES ('k0010'), ('k0020'), ('missing');
ANALYZE probe;

-- encourage the parameterized nested loop
SET enable_mergejoin = off;
SET enable_hashjoin = off;

SELECT p.k, j.value
FROM probe p JOIN j_kv j ON j.key = p.k
ORDER BY p.k;

-- correlated subquery (also drives a per-row key lookup)
SELECT p.k, (SELECT value FROM j_kv j WHERE j.key = p.k) AS value
FROM probe p
ORDER BY p.k;

RESET enable_mergejoin;
RESET enable_hashjoin;

DELETE FROM j_kv;
DROP FOREIGN TABLE j_kv;

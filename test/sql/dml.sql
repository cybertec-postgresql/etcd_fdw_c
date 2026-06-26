-- dml: INSERT / UPDATE / DELETE through the FDW
CREATE FOREIGN TABLE d_kv (key text COLLATE "C", value text, version bigint)
  SERVER etcd OPTIONS (prefix '/test/dml/', strip_prefix 'true');

DELETE FROM d_kv;

-- INSERT
INSERT INTO d_kv (key, value) VALUES ('k1', 'v1'), ('k2', 'v2');
SELECT key, value, version FROM d_kv ORDER BY key;

-- UPDATE value
UPDATE d_kv SET value = 'v1b' WHERE key = 'k1';
SELECT key, value FROM d_kv WHERE key = 'k1';

-- UPDATE key (rename: put new + delete old in one txn)
UPDATE d_kv SET key = 'k3' WHERE key = 'k2';
SELECT key, value FROM d_kv ORDER BY key;

-- DELETE
DELETE FROM d_kv WHERE key = 'k1';
SELECT key, value FROM d_kv ORDER BY key;

-- multi-row INSERT ... SELECT
INSERT INTO d_kv (key, value) SELECT 'm' || g, 'mv' || g FROM generate_series(1, 4) g;
SELECT count(*) FROM d_kv;

-- COPY FROM (exercises BeginForeignInsert)
COPY d_kv (key, value) FROM stdin;
c1	cv1
c2	cv2
\.
SELECT key, value FROM d_kv WHERE key LIKE 'c%' ORDER BY key;

-- batch insert across the 100-op batch boundary (250 rows)
DELETE FROM d_kv;
INSERT INTO d_kv (key, value) SELECT lpad(g::text, 4, '0'), 'v' || g
  FROM generate_series(1, 250) g;
SELECT count(*) FROM d_kv;
SELECT key, value FROM d_kv WHERE key IN ('0001', '0100', '0101', '0250') ORDER BY key;

-- TRUNCATE clears the whole prefix
TRUNCATE d_kv;
SELECT count(*) FROM d_kv;

DROP FOREIGN TABLE d_kv;

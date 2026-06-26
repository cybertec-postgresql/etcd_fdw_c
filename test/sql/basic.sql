-- basic: server/table creation and simple reads/writes
-- (etcd must be reachable at http://etcd:2379)
CREATE SERVER IF NOT EXISTS etcd FOREIGN DATA WRAPPER etcd_fdw
  OPTIONS (endpoints 'http://etcd:2379');

CREATE FOREIGN TABLE b_kv (
  key text COLLATE "C",
  value text,
  create_revision bigint,
  mod_revision bigint,
  version bigint,
  lease bigint
) SERVER etcd OPTIONS (prefix '/test/basic/', strip_prefix 'true');

-- start from a clean prefix so version numbers are deterministic
DELETE FROM b_kv;
INSERT INTO b_kv (key, value) VALUES ('alpha', '1'), ('beta', '2'), ('gamma', '3');

SELECT key, value, version FROM b_kv ORDER BY key;
SELECT count(*) FROM b_kv;

-- revision columns are populated (values are non-deterministic, so just check
-- that they are present and ordered consistently)
SELECT key, (create_revision > 0) AS has_crev, (mod_revision > 0) AS has_mrev
FROM b_kv ORDER BY key;

-- strip_prefix = false exposes the full etcd key
CREATE FOREIGN TABLE b_full (key text COLLATE "C", value text)
  SERVER etcd OPTIONS (prefix '/test/basic/');
SELECT key, value FROM b_full ORDER BY key;

-- value as bytea
CREATE FOREIGN TABLE b_bytea (key text COLLATE "C", value bytea)
  SERVER etcd OPTIONS (prefix '/test/basic/', strip_prefix 'true');
SELECT key, value FROM b_bytea ORDER BY key;

-- whole-keyspace table (empty prefix): predicate pushdown and full scan
CREATE FOREIGN TABLE b_all (key text COLLATE "C", value text)
  SERVER etcd OPTIONS (prefix '');
SELECT key, value FROM b_all WHERE key = '/test/basic/alpha';
SELECT key FROM b_all WHERE key LIKE '/test/basic/%' ORDER BY key;
SELECT count(*) >= 3 AS full_scan_ok FROM b_all;
DROP FOREIGN TABLE b_all;

DELETE FROM b_kv;
DROP FOREIGN TABLE b_kv, b_full, b_bytea;

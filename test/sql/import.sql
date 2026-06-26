-- import: IMPORT FOREIGN SCHEMA discovers child prefixes as tables
CREATE FOREIGN TABLE i_seed (key text COLLATE "C", value text)
  SERVER etcd OPTIONS (prefix '/test/imp/', strip_prefix 'true');

DELETE FROM i_seed;
INSERT INTO i_seed (key, value) VALUES
  ('svc1/host', 'h1'), ('svc1/port', 'p1'),
  ('svc2/host', 'h2'), ('svc2/port', 'p2');

CREATE SCHEMA imp;
IMPORT FOREIGN SCHEMA "/test/imp/" FROM SERVER etcd INTO imp;

SELECT foreign_table_name
FROM information_schema.foreign_tables
WHERE foreign_table_schema = 'imp'
ORDER BY 1;

SELECT key, value FROM imp.svc1 ORDER BY key;
SELECT key, value FROM imp.svc2 ORDER BY key;

DROP SCHEMA imp CASCADE;
DELETE FROM i_seed;
DROP FOREIGN TABLE i_seed;

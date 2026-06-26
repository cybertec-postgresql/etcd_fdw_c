-- lease: grant / ttl / attach / keepalive / revoke lifecycle.
-- Lease ids are non-deterministic, so capture with \gset and assert booleans.
CREATE FOREIGN TABLE l_kv (key text COLLATE "C", value text, lease bigint)
  SERVER etcd OPTIONS (prefix '/test/lease/', strip_prefix 'true');
DELETE FROM l_kv;

SELECT etcd_fdw_lease_grant('etcd', 60) AS lid \gset
SELECT :lid > 0 AS granted;
SELECT etcd_fdw_lease_ttl('etcd', :lid) BETWEEN 1 AND 60 AS ttl_ok;

-- attach a key to the lease
INSERT INTO l_kv (key, value, lease) VALUES ('e1', 'v', :lid);
SELECT key, value FROM l_kv ORDER BY key;

-- refresh it
SELECT etcd_fdw_lease_keepalive('etcd', :lid) BETWEEN 1 AND 60 AS ka_ok;

-- revoke -> attached key is removed; ttl becomes -1
SELECT etcd_fdw_lease_revoke('etcd', :lid) AS revoked;
SELECT count(*) AS remaining FROM l_kv;
SELECT etcd_fdw_lease_ttl('etcd', :lid) AS ttl_after_revoke;

DROP FOREIGN TABLE l_kv;

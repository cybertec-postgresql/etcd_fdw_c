/* etcd_fdw--1.0--1.1.sql */

-- TLS certificate inspection functions and monitoring view.

CREATE FUNCTION etcd_fdw_certificates(server text)
RETURNS TABLE(source     text,
             subject     text,
             issuer      text,
             not_before  timestamptz,
             not_after   timestamptz,
             expires_in  interval)
AS 'MODULE_PATHNAME', 'etcd_fdw_certificates'
LANGUAGE C STRICT;

CREATE FUNCTION etcd_fdw_server_cert_expiry(server text)
RETURNS timestamptz
AS 'MODULE_PATHNAME', 'etcd_fdw_server_cert_expiry'
LANGUAGE C STRICT;

-- Convenience view over every etcd_fdw server.  The WHERE clause restricts the
-- servers the function is invoked for to those the caller may inspect (the C
-- functions enforce the same rule: superuser, pg_monitor, or USAGE on server).
CREATE VIEW etcd_fdw_cert_expiry AS
  SELECT s.srvname AS server, c.source, c.subject, c.issuer,
         c.not_before, c.not_after, c.expires_in
  FROM pg_foreign_server s
  JOIN pg_foreign_data_wrapper w ON w.oid = s.srvfdw AND w.fdwname = 'etcd_fdw'
  CROSS JOIN LATERAL etcd_fdw_certificates(s.srvname) AS c
  WHERE has_server_privilege(current_user, s.oid, 'USAGE')
     OR pg_has_role(current_user, 'pg_monitor', 'USAGE');

GRANT EXECUTE ON FUNCTION etcd_fdw_certificates(text) TO PUBLIC;
GRANT EXECUTE ON FUNCTION etcd_fdw_server_cert_expiry(text) TO PUBLIC;
GRANT SELECT ON etcd_fdw_cert_expiry TO PUBLIC;

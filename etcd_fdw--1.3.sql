/* etcd_fdw--1.3.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION etcd_fdw" to load this file. \quit

CREATE FUNCTION etcd_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION etcd_fdw_validator(text[], oid)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER etcd_fdw
  HANDLER etcd_fdw_handler
  VALIDATOR etcd_fdw_validator;

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

CREATE FUNCTION etcd_fdw_disconnect()
RETURNS integer
AS 'MODULE_PATHNAME', 'etcd_fdw_disconnect'
LANGUAGE C STRICT;

GRANT EXECUTE ON FUNCTION etcd_fdw_disconnect() TO PUBLIC;
-- Lease management.  Functions self-check authorization (superuser or USAGE on
-- the server), so EXECUTE is granted to PUBLIC.

CREATE FUNCTION etcd_fdw_lease_grant(server text, ttl bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'etcd_fdw_lease_grant'
LANGUAGE C STRICT;

CREATE FUNCTION etcd_fdw_lease_revoke(server text, lease bigint)
RETURNS boolean
AS 'MODULE_PATHNAME', 'etcd_fdw_lease_revoke'
LANGUAGE C STRICT;

CREATE FUNCTION etcd_fdw_lease_keepalive(server text, lease bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'etcd_fdw_lease_keepalive'
LANGUAGE C STRICT;

CREATE FUNCTION etcd_fdw_lease_ttl(server text, lease bigint)
RETURNS bigint
AS 'MODULE_PATHNAME', 'etcd_fdw_lease_ttl'
LANGUAGE C STRICT;

GRANT EXECUTE ON FUNCTION etcd_fdw_lease_grant(text, bigint) TO PUBLIC;
GRANT EXECUTE ON FUNCTION etcd_fdw_lease_revoke(text, bigint) TO PUBLIC;
GRANT EXECUTE ON FUNCTION etcd_fdw_lease_keepalive(text, bigint) TO PUBLIC;
GRANT EXECUTE ON FUNCTION etcd_fdw_lease_ttl(text, bigint) TO PUBLIC;

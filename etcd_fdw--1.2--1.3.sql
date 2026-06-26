/* etcd_fdw--1.2--1.3.sql */

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

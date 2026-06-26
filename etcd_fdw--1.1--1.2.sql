/* etcd_fdw--1.1--1.2.sql */

-- Drop this backend's cached etcd connections (forces reconnect, re-reading
-- TLS cert files and re-authenticating).  Handy after an in-place cert rotation.
CREATE FUNCTION etcd_fdw_disconnect()
RETURNS integer
AS 'MODULE_PATHNAME', 'etcd_fdw_disconnect'
LANGUAGE C STRICT;

GRANT EXECUTE ON FUNCTION etcd_fdw_disconnect() TO PUBLIC;

# etcd_fdw — PGXS Makefile
#
# Build against any PostgreSQL 14-18 by pointing PG_CONFIG at the right
# pg_config (defaults to whatever is first on PATH):
#
#   make PG_CONFIG=/usr/lib/postgresql/16/bin/pg_config
#   make install
#   make installcheck            # runs the pg_regress suites (needs a live etcd)

MODULE_big = etcd_fdw

# Vendored cJSON + our sources
OBJS = \
	src/etcd_fdw.o \
	src/options.o \
	src/deparse.o \
	src/etcd_json.o \
	src/etcd_conn.o \
	src/etcd_client.o \
	src/etcd_cert.o \
	src/etcd_lease.o \
	third_party/cJSON/cJSON.o

EXTENSION = etcd_fdw
DATA = etcd_fdw--1.0.sql etcd_fdw--1.1.sql etcd_fdw--1.2.sql etcd_fdw--1.3.sql etcd_fdw--1.0--1.1.sql etcd_fdw--1.1--1.2.sql etcd_fdw--1.2--1.3.sql
PGFILEDESC = "etcd_fdw - foreign-data wrapper for etcd v3"

# libcurl for HTTP/JSON transport
PG_CPPFLAGS += -I$(CURDIR)/src -I$(CURDIR)/third_party/cJSON
SHLIB_LINK += $(shell curl-config --libs 2>/dev/null || echo -lcurl)
PG_CPPFLAGS += $(shell curl-config --cflags 2>/dev/null)
# OpenSSL (libcrypto) for parsing local CA/client certificate files
SHLIB_LINK += $(shell pkg-config --libs libcrypto 2>/dev/null || echo -lcrypto)
PG_CPPFLAGS += $(shell pkg-config --cflags libcrypto 2>/dev/null)

# Regression suites. Require a reachable etcd (see test/scripts).
REGRESS = basic pushdown dml join lease import analyze errors
REGRESS_OPTS = --inputdir=test --outputdir=test --load-extension=etcd_fdw

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

# Build etcd_fdw against a chosen PostgreSQL major version.
#
#   docker build -f test/docker/Dockerfile --build-arg PG_MAJOR=16 -t etcd_fdw:pg16 .
#
# The resulting image is a postgres:<PG_MAJOR> with the extension built and
# installed, ready for `make installcheck` against a reachable etcd.
ARG PG_MAJOR=16
FROM postgres:${PG_MAJOR}
ARG PG_MAJOR

RUN apt-get update \
 && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential \
        postgresql-server-dev-${PG_MAJOR} \
        libcurl4-openssl-dev \
        libssl-dev \
 && rm -rf /var/lib/apt/lists/*

COPY . /src
WORKDIR /src
RUN make clean && make && make install && chown -R postgres /src

# Aster runtime image: BusyBox (musl) + fully-static Aster binary.
#
# Build (from repo root):
#   ./scripts/docker-build.sh
#
# The binary is cross-compiled with toolchains_musl so it has zero shared
# library dependencies and runs on busybox:*-musl and scratch alike.

# syntax=docker/dockerfile:1.7

ARG BUSYBOX_IMAGE=busybox:1.37.0-musl

FROM ${BUSYBOX_IMAGE}

LABEL org.opencontainers.image.title="Aster" \
      org.opencontainers.image.description="Peer-to-peer vector database (BusyBox musl runtime)" \
      org.opencontainers.image.source="https://github.com/cenkcorapci/Aster" \
      org.opencontainers.image.licenses="AGPL-3.0"

# Non-root runtime identity (uid/gid 1000). Entrypoint starts as root only
# long enough to chown the data volume, then drops to this user.
RUN addgroup -g 1000 aster \
 && adduser -D -H -u 1000 -G aster aster \
 && mkdir -p /data \
 && chown aster:aster /data

COPY aster /usr/local/bin/aster
COPY su-exec /usr/local/sbin/su-exec
COPY entrypoint.sh /entrypoint.sh
RUN chmod 755 /usr/local/bin/aster /usr/local/sbin/su-exec /entrypoint.sh

ENV ASTER_DATA_DIR=/data \
    ASTER_BIN=/usr/local/bin/aster

VOLUME ["/data"]
WORKDIR /data

# Root for volume bootstrap; entrypoint drops to uid 1000.
USER root
ENTRYPOINT ["/entrypoint.sh"]
# Default: durable single-node demo against /data.
CMD []

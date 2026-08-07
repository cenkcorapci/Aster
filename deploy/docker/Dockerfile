# Aster runtime image: BusyBox (musl) + static aster + aster-bench.
#
# Build: ./scripts/docker-build.sh

# syntax=docker/dockerfile:1.7

ARG BUSYBOX_IMAGE=busybox:1.37.0-musl

FROM ${BUSYBOX_IMAGE}

LABEL org.opencontainers.image.title="Aster" \
      org.opencontainers.image.description="Peer-to-peer vector database (BusyBox musl runtime)" \
      org.opencontainers.image.source="https://github.com/cenkcorapci/Aster" \
      org.opencontainers.image.licenses="AGPL-3.0"

RUN addgroup -g 1000 aster \
 && adduser -D -H -u 1000 -G aster aster \
 && mkdir -p /data /results \
 && chown aster:aster /data /results

COPY aster /usr/local/bin/aster
COPY aster-bench /usr/local/bin/aster-bench
COPY su-exec /usr/local/sbin/su-exec
COPY entrypoint.sh /entrypoint.sh
RUN chmod 755 /usr/local/bin/aster /usr/local/bin/aster-bench \
              /usr/local/sbin/su-exec /entrypoint.sh

ENV ASTER_DATA_DIR=/data \
    ASTER_BIN=/usr/local/bin/aster \
    ASTER_BENCH_BIN=/usr/local/bin/aster-bench \
    ASTER_MODE=demo

VOLUME ["/data", "/results"]
WORKDIR /data

USER root
ENTRYPOINT ["/entrypoint.sh"]
CMD []

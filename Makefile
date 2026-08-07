# Aster developer shortcuts.
#
# Bench (local kind + 50 BusyBox nodes, mixed write/update/search):
#   make bench-local    # embedded / emptyDir storage
#   make bench-minio    # + MinIO S3 simulation with mc sync sidecars
#   make bench-clean    # delete kind cluster + namespace leftovers
#
# Optional overrides:
#   make bench-local NODES=20 DURATION=60 TARGET_VECTORS=100000000

.PHONY: help bench-local bench-minio bench-clean docker-image test

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
NODES ?= 50
TARGET_VECTORS ?= 100000000
DIMENSION ?= 16
DURATION ?= 180
IMAGE ?= aster:local
CLUSTER ?= aster-bench
export NODES TARGET_VECTORS DIMENSION DURATION IMAGE CLUSTER
export FORCE_COLOR=1

help:
	@printf '%s\n' \
		'Aster Makefile' \
		'' \
		'  make bench-local     50-node kind soak, local/embedded storage' \
		'  make bench-minio     50-node kind soak + MinIO (S3 stand-in)' \
		'  make bench-clean     delete kind cluster $(CLUSTER)' \
		'  make docker-image    build BusyBox aster + aster-bench image' \
		'  make test            bazel test //aster/...' \
		'' \
		'Overrides: NODES=50 TARGET_VECTORS=100000000 DIMENSION=16 DURATION=180'

docker-image:
	./scripts/docker-build.sh

test:
	bazel test //aster/...

bench-local: docker-image
	FORCE_COLOR=1 ./deploy/bench/run.sh local

bench-minio: docker-image
	FORCE_COLOR=1 ./deploy/bench/run.sh minio

bench-clean:
	-kind delete cluster --name "$(CLUSTER)"
	@echo "kind cluster $(CLUSTER) removed (if it existed)"

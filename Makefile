# Aster developer shortcuts.
#
# Bench (local kind + BusyBox nodes, mixed write/update/search):
#   make bench-local              # dims 256, 2048, 4096 (default matrix)
#   make bench-minio              # same + MinIO S3 simulation
#   make bench-clean              # delete kind cluster
#   make bench-test               # fast local dim smoke (bazel)
#
# Optional overrides:
#   make bench-local DIMENSIONS="256 2048" NODES=10 DURATION=60
#   make bench-local DIMENSION=512   # single dim (overrides DIMENSIONS)

.PHONY: help bench-local bench-minio bench-clean bench-test docker-image test

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
NODES ?= 50
TARGET_VECTORS ?= 100000000
# Preferred: space-separated matrix. Single DIMENSION= still works.
DIMENSIONS ?= 256 2048 4096
DIMENSION ?=
DURATION ?= 180
IMAGE ?= aster:local
CLUSTER ?= aster-bench
export NODES TARGET_VECTORS DIMENSIONS DIMENSION DURATION IMAGE CLUSTER
export FORCE_COLOR=1

help:
	@printf '%s\n' \
		'Aster Makefile' \
		'' \
		'  make bench-local     kind soak for dims $(DIMENSIONS)' \
		'  make bench-minio     kind soak + MinIO for dims $(DIMENSIONS)' \
		'  make bench-test      bazel micro-bench for 256/2048/4096' \
		'  make bench-clean     delete kind cluster $(CLUSTER)' \
		'  make docker-image    build BusyBox aster + aster-bench image' \
		'  make test            bazel test //aster/...' \
		'' \
		'Overrides: DIMENSIONS="256 2048 4096" DIMENSION=512 NODES=50 DURATION=180'

docker-image:
	./scripts/docker-build.sh

test:
	bazel test //aster/...

bench-test:
	bazel test //aster/bench:bench_test --test_output=all

bench-local: docker-image
	FORCE_COLOR=1 ./deploy/bench/run.sh local

bench-minio: docker-image
	FORCE_COLOR=1 ./deploy/bench/run.sh minio

bench-clean:
	-kind delete cluster --name "$(CLUSTER)"
	@echo "kind cluster $(CLUSTER) removed (if it existed)"

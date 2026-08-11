# Aster developer shortcuts.
#
# Bench (local kind + BusyBox nodes, mixed write/update/search):
#   make bench-local              # dims 256, 2048, 4096 (default matrix)
#   make bench-minio              # same + MinIO S3 simulation
#   make bench-clean              # delete kind cluster
#   make bench-test               # fast local dim smoke (bazel)
#
# Multi-tenant Catalog (mixed dims × row sizes):
#   make bench-multitenant-test   # gtest smoke
#   make bench-multitenant-smoke  # 3 tenants × 4 index sizes
#   make bench-multitenant        # 8 tenants × 9 sizes
#   make bench-multitenant-large  # 12 tenants × larger corpora
#
# Elastic scale (15↔50, RF=2, local + minio object stand-in):
#   make bench-scale-test         # ring rebalance unit tests
#   make bench-scale-smoke        # 3↔8 quick
#   make bench-scale              # 15↔50 local then minio backends
#
# Live simulation + Grafana (MinIO, scale, 50 indexes → 1B target):
#   make sim-grafana              # boot kind + Prometheus + Grafana
#   make sim-grafana-stop         # tear down
#
# Arduino / ESP32 firmware-in-emulator (PlatformIO + Espressif QEMU):
#   make sim-arduino              # build Tiny embedded firmware + QEMU ASTER_OK
#   make sim-arduino-native       # host smoke (no MCU)
#   make sim-arduino-clean
#
# Aster vs Milvus (Aster shards + Milvus standalone + MinIO, target 100M × 2048):
#   make bench-vs-milvus
#   make bench-vs-milvus-smoke
#   make bench-vs-milvus-clean

.PHONY: help bench-local bench-minio bench-clean bench-test docker-image test \
	bench-vs-milvus bench-vs-milvus-smoke bench-vs-milvus-clean \
	bench-multitenant bench-multitenant-smoke bench-multitenant-large \
	bench-multitenant-test \
	bench-scale bench-scale-smoke bench-scale-test \
	sim-grafana sim-grafana-stop \
	sim-arduino sim-arduino-native sim-arduino-build sim-arduino-clean

ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
NODES ?= 50
TARGET_VECTORS ?= 100000000
DIMENSIONS ?= 256 2048 4096
DIMENSION ?=
DURATION ?= 180
IMAGE ?= aster:local
CLUSTER ?= aster-bench
TOP_K ?= 10
QUERIES ?= 50
MAX_NODES ?= 50
MIN_NODES ?= 15
START_NODES ?= 25
INDEXES ?= 50
TARGET_TOTAL_ROWS ?= 1000000000
SCALE_EVERY_SEC ?= 45
WORK_RPS ?= 20
export NODES TARGET_VECTORS DIMENSIONS DIMENSION DURATION IMAGE CLUSTER TOP_K QUERIES
export TENANTS CONCURRENT
export MAX_NODES MIN_NODES START_NODES INDEXES TARGET_TOTAL_ROWS SCALE_EVERY_SEC WORK_RPS
export FORCE_COLOR=1

help:
	@printf '%s\n' \
		'Aster Makefile' \
		'' \
		'  make bench-local               kind soak for dims $(DIMENSIONS)' \
		'  make bench-minio               kind soak + MinIO for dims $(DIMENSIONS)' \
		'  make bench-test                bazel micro-bench for 256/2048/4096' \
		'  make bench-clean               delete kind cluster $(CLUSTER)' \
		'  make bench-multitenant-test    multi-tenant Catalog gtest' \
		'  make bench-multitenant-smoke   3 tenants × mixed dim/rows' \
		'  make bench-multitenant         8 tenants × 9 dim/row indexes' \
		'  make bench-multitenant-large   12 tenants × larger corpora' \
		'  make bench-scale-test          rebalance / RF unit tests' \
		'  make bench-scale-smoke         elastic scale 3↔8' \
		'  make bench-scale               elastic scale 15↔50 (local+minio)' \
		'  make sim-grafana               live 50-node MinIO sim + Grafana' \
		'  make sim-grafana-stop          tear down aster-sim kind cluster' \
		'  make sim-arduino               ESP32 Tiny firmware + Espressif QEMU' \
		'  make sim-arduino-native        host smoke of Arduino harness' \
		'  make sim-arduino-clean         clean PlatformIO / flash cache' \
		'  make bench-vs-milvus           Aster vs Milvus (100M×2048 target)' \
		'  make bench-vs-milvus-smoke     tiny Aster vs Milvus smoke' \
		'  make bench-vs-milvus-clean     delete kind cluster aster-vs-milvus' \
		'  make docker-image              build BusyBox aster + aster-bench image' \
		'  make test                      bazel test //aster/...' \
		'' \
		'Overrides: MAX_NODES MIN_NODES START_NODES INDEXES TARGET_TOTAL_ROWS' \
		'           SCALE_EVERY_SEC WORK_RPS DIMENSIONS TENANTS QUERIES'

docker-image:
	./scripts/docker-build.sh

test:
	bazel test //aster/...

bench-test:
	bazel test //aster/bench:bench_test --test_output=all

bench-multitenant-test:
	bazel test //aster/bench:multi_tenant_bench_test --test_output=all

bench-scale-test:
	bazel test //aster/distributed:distributed_test --test_output=all

bench-local: docker-image
	FORCE_COLOR=1 ./deploy/bench/run.sh local

bench-minio: docker-image
	FORCE_COLOR=1 ./deploy/bench/run.sh minio

bench-clean:
	-kind delete cluster --name "$(CLUSTER)"
	@echo "kind cluster $(CLUSTER) removed (if it existed)"

bench-multitenant-smoke:
	./deploy/bench-multitenant/run.sh smoke

bench-multitenant:
	./deploy/bench-multitenant/run.sh default

bench-multitenant-large:
	./deploy/bench-multitenant/run.sh large

bench-scale-smoke:
	./deploy/bench-scale/run.sh smoke both

bench-scale:
	./deploy/bench-scale/run.sh full both

sim-grafana: docker-image
	SIM_CLUSTER=aster-sim FORCE_COLOR=1 ./deploy/sim-grafana/run.sh start

sim-grafana-stop:
	SIM_CLUSTER=aster-sim ./deploy/sim-grafana/run.sh stop

sim-arduino:
	FORCE_COLOR=1 ./deploy/sim-arduino/run.sh all

sim-arduino-native:
	FORCE_COLOR=1 ./deploy/sim-arduino/run.sh native

sim-arduino-build:
	FORCE_COLOR=1 ./deploy/sim-arduino/run.sh build

sim-arduino-clean:
	./deploy/sim-arduino/run.sh clean

bench-vs-milvus: docker-image
	# Unset NODES so the global NODES=50 export cannot inflate Aster shards.
	env -u NODES CLUSTER=aster-vs-milvus COMPARE_CLUSTER=aster-vs-milvus \
		FORCE_COLOR=1 ./deploy/compare-milvus/run.sh full

bench-vs-milvus-smoke: docker-image
	env -u NODES CLUSTER=aster-vs-milvus COMPARE_CLUSTER=aster-vs-milvus \
		FORCE_COLOR=1 ./deploy/compare-milvus/run.sh smoke

bench-vs-milvus-clean:
	-kind delete cluster --name aster-vs-milvus
	@echo "kind cluster aster-vs-milvus removed (if it existed)"

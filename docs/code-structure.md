Aster is a strong choice because it is short, memorable, and already evokes a distributed “star” topology without being tied to vectors specifically. Given your deployment targets (Arduino-class MCUs, ARM SBCs, Intel servers, Docker containers, and S3-backed cloud storage), I would design Aster as a layered portability-first C++ codebase where the core engine has no OS assumptions, and storage, networking, compression, metrics, and threading are pluggable.

The most important design decision is: separate the database kernel from the runtime. The kernel should compile on bare metal, while Linux/macOS/cloud builds simply provide richer implementations of the same interfaces.

### Aster architecture

### Design principles

* C++20 core

* Zero mandatory runtime dependencies

* Header-only where possible

* No exceptions in hot paths

* Deterministic memory usage

* Pluggable storage backend

* Pluggable networking backend

* Pluggable threading backend

* Cross-endian safe

* Little-endian optimized

* Single binary deployment

### Layered architecture

```
+------------------------------------------------------+
| Aster API / Thrift RPC                               |
+------------------------------------------------------+
| Query Engine                                         |
|  - vector search                                     |
|  - id lookup                                         |
|  - tag filtering                                     |
+------------------------------------------------------+
| Distributed Layer                                    |
|  - gossip                                             |
|  - vnode ownership                                    |
|  - replication                                        |
+------------------------------------------------------+
| Storage Engine                                       |
|  - memtable                                          |
|  - WAL                                               |
|  - SSTable                                           |
|  - HNSW segments                                     |
+------------------------------------------------------+
| Platform Abstraction Layer                           |
|  - file system                                       |
|  - mmap                                              |
|  - clock                                             |
|  - threads                                           |
|  - sockets                                           |
|  - compression                                       |
+------------------------------------------------------+
| Platform Backend                                     |
|  Linux | Windows | macOS | ARM | Arduino | Baremetal |
+------------------------------------------------------+
```

Everything above the Platform Abstraction Layer is identical across all targets.

### Build targets

| Target       | Backend                          |
| ------------ | -------------------------------- |
| Arduino      | Static allocation + EEPROM/Flash |
| ESP32        | LittleFS / SPIFFS                |
| Raspberry Pi | POSIX                            |
| ARM Linux    | POSIX                            |
| x86 Linux    | POSIX + mmap                     |
| Docker       | POSIX + epoll                    |
| S3 Cloud     | S3 object backend                |

### Core portability layer

### Filesystem interface

C++

```
class IStorageBackend {
public:
    virtual Result<void> write(
        const std::string& path,
        const Buffer& data) = 0;

    virtual Result<Buffer> read(
        const std::string& path) = 0;

    virtual Result<void> remove(
        const std::string& path) = 0;

    virtual Result<std::vector<std::string>>
        list(const std::string& prefix) = 0;

    virtual bool exists(
        const std::string& path) = 0;

    virtual ~IStorageBackend() = default;
};
```

Implementations:

* PosixStorage

* ArduinoStorage

* LittleFSStorage

* S3Storage

* MemoryStorage

The storage engine never knows whether data is on a local disk or S3.

### S3 storage backend

S3 should be treated as object storage, not a filesystem.

### Layout

```
aster/
    table_products/
        manifest.json
        wal/
            000001.log
        segments/
            seg_000001.ast
            seg_000002.ast
        index/
            seg_000001.hnsw
            seg_000002.hnsw
```

Objects are immutable.

Write flow:

1. create local temporary object

2. upload

3. update manifest atomically

For cloud deployment:

* multipart upload

* range GET

* async prefetch

* local block cache

### Read cache

S3 backend keeps:

* LRU block cache

* manifest cache

* HNSW upper layers cached locally

This prevents excessive S3 latency.

### Memory model

### Embedded mode

No heap allocations during normal operation.

Use:

C++

```
template<size_t N>
class StaticArena {
public:
    void* allocate(size_t size);
    void reset();

private:
    alignas(std::max_align_t) std::array<std::byte, N> memory_;
    size_t offset_ = 0;
};
```

Compile-time arena sizes:

C++

```
AsterConfig {
    .memtable_size = 128 * 1024,
    .arena_size    = 512 * 1024,
    .cache_size    = 256 * 1024
};
```

### Server mode

Use slab allocator + arena allocation.

Benefits:

* minimal fragmentation

* deterministic performance

* fast reset after flush

### WAL design

Portable binary format.

```
[magic]
[version]
[crc]
[length]
[row payload]
```

Append-only.

Sync policies:

* ALWAYS

* EVERY_MS

* NEVER

Embedded devices can choose NEVER or EVERY_MS.

### SSTable format

Single immutable file.

```
+-----------------------------+
| Header                      |
+-----------------------------+
| Bloom Filter                |
+-----------------------------+
| Sparse Index                |
+-----------------------------+
| ID Index                    |
+-----------------------------+
| Vector Block                |
+-----------------------------+
| Metadata Block              |
+-----------------------------+
| Tag Bitmap Block            |
+-----------------------------+
| Tree Block                  |
+-----------------------------+
| Footer                      |
+-----------------------------+
```

Blocks are independently compressed.

### Compression

Compile-time selectable.

C++

```
enum class Compression {
    None,
    LZ4,
    ZSTD
};
```

Recommended:

* Arduino: None

* ESP32: LZ4

* ARM: LZ4

* x86: ZSTD level 1

* Cloud: ZSTD level 3

### SIMD abstraction

Distance computation must compile everywhere.

C++

```
class DistanceKernel {
public:
    virtual float l2(
        const float* a,
        const float* b,
        size_t dim) const = 0;
};
```

Implementations:

* ScalarKernel

* NeonKernel

* AVX2Kernel

* AVX512Kernel

Selected at runtime or compile time.

### Threading abstraction

### Interface

C++

```
class IExecutor {
public:
    virtual void submit(Task task) = 0;
    virtual void wait() = 0;
};
```

Backends:

* StdThreadPool

* PosixThreadPool

* FreeRTOSExecutor

* SingleThreadExecutor

Arduino uses SingleThreadExecutor.

### Networking abstraction

C++

```
class ITransport {
public:
    virtual Result<void> send(
        const Endpoint&,
        const Buffer&) = 0;

    virtual Result<Buffer> receive() = 0;
};
```

Implementations:

* TCP

* UDP

* QUIC (optional)

* Serial (Arduino)

This allows clusters of embedded devices.

### Runtime profiles

### Tiny profile

For Arduino / ESP32.

```
Features:
- no HNSW
- exact search
- no replication
- no gossip
- no compression
- single thread
Memory:
- <1 MB
```

### Edge profile

For Raspberry Pi / ARM gateways.

```
Features:
- HNSW
- gossip
- replication
- LZ4
- mmap optional
Memory:
- 32-128 MB
```

### Server profile

For Intel / cloud.

```
Features:
- full HNSW
- compaction
- S3 backend
- Prometheus
- Thrift
- SIMD
- thread pools
Memory:
- configurable
```

### Docker deployment

Minimal container.

dockerfile

```
FROM scratch

COPY aster /aster

ENTRYPOINT ["/aster"]
```

Static linking.

Target image:

* <15 MB

Alternative:

* distroless

* alpine

### Configuration

Single TOML file.

TOML

```
[cluster]
node_id = "node-1"
vnodes = 256
replication_factor = 3

[storage]
backend = "s3"
bucket = "aster-prod"
cache_size_mb = 512

[index]
hnsw_m = 16
ef_construct = 128

[metrics]
prometheus = true
port = 9100
```

### S3 optimization

Do not store one object per row.

Instead:

* immutable segment files

* object size 16-128 MB

* append via new segment

* compaction creates new object

* old objects garbage collected

This aligns perfectly with LSM semantics.

### Recommended code structure

```
aster/
├── core/
│   ├── types/
│   ├── memory/
│   ├── io/
│   └── util/
├── storage/
│   ├── wal/
│   ├── memtable/
│   ├── sstable/
│   └── manifest/
├── index/
│   ├── hnsw/
│   ├── tags/
│   └── bloom/
├── distributed/
│   ├── gossip/
│   ├── vnode/
│   ├── replication/
│   └── repair/
├── query/
│   ├── search/
│   ├── filter/
│   └── planner/
├── rpc/
│   ├── thrift/
│   └── protocol/
├── platform/
│   ├── posix/
│   ├── windows/
│   ├── arduino/
│   ├── freertos/
│   └── s3/
├── metrics/
├── cli/
└── tests/
```

### The key architectural decision

I would make Aster's storage backend object-oriented and immutable. Every write eventually becomes an immutable segment, whether stored on:

* SSD

* HDD

* SPI Flash

* SD card

* EEPROM

* S3 object storage

That gives you one storage engine that naturally scales from an ESP32 with 8 MB flash to a Kubernetes deployment using S3 as the primary storage layer, without changing the higher-level database code.

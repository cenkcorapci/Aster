This scenario changes the architecture significantly. The previous Aster design was a **general-purpose distributed database**. This version should be designed as a **cloud-native, serverless-ish vector database optimized for spot instances + S3 economics**.

The core insight:

> Do not fight ephemeral compute. Make compute disposable and make S3 the source of truth.

Instead of Cassandra's model (persistent nodes holding data), Aster Cloud should behave more like **a distributed LSM engine where workers are replaceable query/index executors**.

The closest mental models are:

* Snowflake: compute separated from storage
* DynamoDB: managed distributed API
* Firebase: developer experience
* Milvus/Zilliz: vector workloads
* RocksDB: storage engine
* Cassandra: partitioning model

# Aster Cloud Architecture

## Goals

Aster Cloud provides:

* Firebase-like developer API
* Multi-tenant vector database
* Very low operational cost
* Spot-instance optimized Kubernetes deployment
* S3 as durable storage
* Automatic scaling
* Pay-per-use economics
* Vector search with predictable latency
* No persistent database nodes

---

# Core Architecture

```
                  Users
                    |
                    |
             Aster API Gateway
                    |
        +-----------+-----------+
        |                       |
 Tenant Router            Auth/Billing
        |
        |
 Query Coordinator Layer
        |
+-------+--------+
|                |
Search Workers   Write Workers
(ephemeral)      (ephemeral)
|                |
+-------+--------+
        |
        |
   Aster Storage Layer
        |
        |
       S3
```

Kubernetes nodes are disposable.

The database survives without them.

---

# Storage Architecture

## S3 is the database

S3 stores:

```
aster-bucket/

 tenants/

   tenant-id/

      collections/

          collection-id/

              manifest.json

              segments/

                  segment-00001.ast

                  segment-00002.ast

                  segment-00003.ast

              indexes/

                  hnsw-00001.idx

                  hnsw-00002.idx

              wal/

                  wal-00001.log
```

Everything is immutable.

Updates create new versions.

---

# Write Architecture

## Write path

```
Client
 |
API
 |
Write coordinator
 |
WAL buffer
 |
Memory buffer
 |
Segment builder
 |
Upload to S3
 |
Update manifest
```

Important:

Do not update existing S3 objects.

Only:

* append
* create new objects
* atomically replace manifest

---

# Segment design

A segment contains:

```
Segment

+----------------+
| Vector data    |
+----------------+
| ID index       |
+----------------+
| Metadata       |
+----------------+
| Tags bitmap    |
+----------------+
| HNSW graph     |
+----------------+
| Statistics     |
+----------------+
```

Example:

```
segment-123.ast

Header

Vector block

[1024 vectors]

ID block

[offset table]

Metadata block

[zstd compressed]

HNSW block

[neighbors]
```

---

# Why immutable segments are important

Spot instances disappear.

Therefore:

Bad:

```
Worker
 |
Mutable HNSW
 |
Disk
```

If worker dies:

everything lost.

Good:

```
Worker

Build index

       |
       v

Immutable segment

       |
       v

S3
```

Worker death does not matter.

---

# Compute Architecture

## Worker types

## 1. Query workers

Short lived.

Responsibilities:

* download indexes
* cache segments
* execute searches

Scaling:

CPU based.

Example:

```
1000 searches/sec

        |
        v

increase workers
```

---

## 2. Index workers

Responsible for:

* building HNSW
* merging segments
* compression

Spot optimized.

If interrupted:

restart from checkpoint.

---

## 3. Compaction workers

Background.

Merge:

```
segment A
segment B
segment C

        |

        v

segment ABC
```

---

# Multi tenancy

This is where the economics become interesting.

Aster should not create:

```
tenant A
    pod

tenant B
    pod

tenant C
    pod
```

Too expensive.

Instead:

```
                 Worker Pool

        +-----------------------+

        | Tenant A queries      |
        | Tenant B queries      |
        | Tenant C queries      |

        +-----------------------+
```

---

# Tenant isolation

Every request contains:

```
tenant_id
collection_id
api_key
```

Storage:

```
tenant_id/hash(vector_id)
```

Partitioning:

```
tenant_id
 |
 collection
 |
 vnode
```

---

# Memory management

Use a shared cache.

Example:

```
Node memory:

+----------------------+
| Hot segments         |
| Tenant A             |
| Tenant B             |
| Tenant C             |
+----------------------+
```

LRU eviction.

Priority:

1. paying customers
2. hot collections
3. recent segments

---

# Vector Search Architecture

## Query flow

```
Search request

      |
      v

Tenant router

      |
      v

Find manifest

      |
      v

Find relevant segments

      |
      v

Parallel HNSW search

      |
      v

Merge candidates

      |
      v

Filter tags

      |
      v

Return top K
```

---

# Segment selection optimization

Do not search every segment.

Maintain:

```
Collection Manifest


segment_id

vector_count

min_timestamp

max_timestamp

centroid

bounding information

```

Example:

```
Query vector

      |
      v

Nearest segment centroids

      |
      v

Search only 20/200 segments
```

This becomes a second-level ANN.

---

# Two-level ANN design

This is where Aster could differentiate.

## Level 1

Collection routing index.

Small.

RAM resident.

Example:

```
1000 segment centroids

HNSW

```

Find relevant segments.

---

## Level 2

Segment HNSW.

Actual search.

```
             Collection HNSW

                   |
       +-----------+----------+

       |                      |

 Segment HNSW          Segment HNSW

```

Benefits:

* lower memory
* faster startup
* better S3 utilization

---

# Kubernetes deployment

## Node groups

Use different spot pools.

Example:

```
Query workers

c7i.large
c7a.large


Index workers

r7a.large


Compaction workers

spot only
```

---

# Handling spot interruption

Every worker:

heartbeat:

```
worker_id
last_checkpoint
current_tasks
```

When termination notice:

1. stop accepting requests
2. upload state
3. exit

No data loss because:

* WAL
* S3 segments
* manifests

---

# Metadata database

You still need a small strongly consistent database.

Options:

* DynamoDB
* PostgreSQL
* FoundationDB

Store:

```
Tenant

Collection

API keys

Billing

Manifest pointers

Usage metrics
```

Do NOT put this in S3.

---

# Firebase-like API

Example:

## Create collection

```
POST /v1/projects/demo/collections/products
```

Response:

```
{
 "collection_id":"products",
 "dimension":384
}
```

---

## Insert

```
POST /collections/products/documents
```

Body:

```json
{
"id":"123",
"vector":[0.1,0.2],
"metadata":{
"name":"iphone"
},
"tags":[
"phone"
]
}
```

---

## Search

```
POST /collections/products/search
```

Body:

```json
{
"vector":[0.2,0.4],
"limit":10,
"filter":{
"category":"phone"
}
}
```

---

# Pricing advantage

Traditional vector DB:

```
Always running cluster

3 nodes
24/7

$500+/month
```

Aster:

```
S3:
$20

Compute:
$50

Cache:
$30

Total:
$100
```

For small customers:

Shared compute:

```
1000 tenants
|
same workers
```

This is the advantage.

---

# Cost optimization ideas

## 1. Cold collections

No queries:

Move entirely to S3.

Cost:

almost zero.

## 2. Hot collections

Keep:

* HNSW top layers
* manifests
* recent segments

in RAM.

## 3. Adaptive indexing

Small collections:

Flat search.

Example:

<50k vectors:

```
brute force SIMD
```

Large:

```
HNSW
```

---

# Recommended technology choices

## C++

Core:

* C++20
* coroutine based async

Storage:

* custom segment format
* AWS SDK optional

Compression:

* ZSTD
* LZ4

Networking:

* gRPC internally
* REST externally

Kubernetes:

* Helm
* Kubernetes operators later

Metrics:

* prometheus-cpp

---

# Biggest technical risks

## 1. S3 latency

Solution:

aggressive caching.

---

## 2. Too many tiny segments

Solution:

adaptive compaction.

---

## 3. Tenant noisy neighbors

Solution:

resource scheduler:

```
tenant priority
query budget
memory budget
```

---

## 4. Manifest consistency

Use:

DynamoDB conditional writes.

Example:

```
manifest_version=100

update only if version=100
```

---

# Final architecture summary

Aster Cloud should not be "Cassandra running on Kubernetes".

It should be:

> "A serverless vector database where Kubernetes workers are disposable compute, S3 is the permanent database, and immutable HNSW segments are the unit of storage."

The winning architecture is:

* Cassandra-style partitioning
* S3-backed immutable LSM storage
* two-level HNSW indexing
* spot-instance tolerant workers
* shared multi-tenant compute pools
* Firebase-style developer API

This architecture is much more aligned with building a profitable SaaS because your biggest cost (always-running database clusters) disappears. It also gives Aster a clear positioning: **the low-cost, serverless alternative to Pinecone/Milvus/Weaviate for developers who do not want to manage vector infrastructure.**

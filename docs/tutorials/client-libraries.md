# Tutorial: client libraries

Aster ships seven language clients in `clients/`. They share one public
shape (`connect` → `collection` → upsert / get / delete / search) and one
wire protocol: [`aster/rpc/aster.thrift`](../../aster/rpc/aster.thrift).

**Status (pre-alpha):** facades and package skeletons exist. Transport,
codegen, and a live Thrift server land in milestones **M4–M5**. Calling
data methods today raises “not implemented” (or equivalent). Use the
[C++ engine tutorial](database-management.md) for real reads/writes until
then.

## Packages

| Language | Path | Package |
| --- | --- | --- |
| C++ | `clients/cpp/` | `aster::client` |
| Python | `clients/python/` | `aster-client` |
| Go | `clients/go/` | module under `clients/go` |
| Rust | `clients/rust/` | `aster-client` |
| Java | `clients/java/` | `io.aster:aster-client` |
| Scala | `clients/scala/` | `io.aster:aster-client-scala` |
| JavaScript | `clients/javascript/` | `@aster-db/client` |

Details: [`clients/README.md`](../../clients/README.md).

## Common pattern

```
Client(seeds…)
  └─ collection("name")
        ├─ upsert(id, vector, tags?, metadata?)
        ├─ get(id)
        ├─ delete(id)
        └─ search(vector, top_k?, ef_search?, tags?)
```

- Any seed node can coordinate (Cassandra-style).
- Scores are always **higher is better** for every metric.
- `ef_search` is a per-query recall/latency knob (collection default when omitted).
- `tags` on search are an AND filter (document must contain all listed tags).
- Consistency (`ONE` / `QUORUM` / `ALL`) is accepted in the API for M7+;
  single-node servers treat everything like `ONE`.

## Python

```python
from aster import Client

client = Client(seeds=["127.0.0.1:7000"], tls=False, timeout_ms=5000)
products = client.collection("products")

products.upsert(
    "doc-1",
    [0.1, 0.2, 0.3],
    tags=["electronics"],
    consistency="ONE",
)
hits = products.search([0.1, 0.2, 0.3], top_k=10, ef_search=128, tags=["electronics"])
for hit in hits:
    print(hit.id, hit.score)
```

Install path today: develop from the monorepo (`clients/python`). PyPI
publish is milestone M6.

## Go

```go
client, err := aster.Connect(aster.Options{
    Seeds:     []string{"127.0.0.1:7000"},
    TimeoutMs: 5000,
})
products := client.Collection("products")

err = products.Upsert(ctx, "doc-1", vec, aster.WithTags("electronics"))
hits, err := products.Search(ctx, query,
    aster.TopK(10),
    aster.EfSearch(128),
    aster.WithTags("electronics"),
)
```

## Rust

```rust
use aster_client::{Client, SearchOptions, UpsertOptions};

let client = Client::connect(&["127.0.0.1:7000"]).await?;
let products = client.collection("products");

products
    .upsert("doc-1", &vec, UpsertOptions { /* tags, … */ ..Default::default() })
    .await?;
let hits = products
    .search(&query, SearchOptions { top_k: 10, ef_search: Some(128), ..Default::default() })
    .await?;
```

## JavaScript / TypeScript

```ts
import { AsterClient } from "@aster-db/client";

const client = await AsterClient.connect({ seeds: ["127.0.0.1:7000"] });
const products = client.collection("products");

await products.upsert("doc-1", vector, { tags: ["electronics"] });
const hits = await products.search(query, { topK: 10, efSearch: 128 });
```

## C++

```cpp
#include "aster_client.h"

aster::client::Client client({{.seed_nodes = {"127.0.0.1:7000"}}});
auto products = client.Collection("products");

products.Upsert("doc-1", vec, {.tags = {"electronics"}});
auto hits = products.Search(query, {.top_k = 10, .ef_search = 128});
```

Note: this is the **RPC client** facade (`aster::client`), not the embedded
engine (`aster::Db`). For in-process use, see
[database management](database-management.md).

## Java / Scala

Same collection-centric surface:

```java
AsterClient client = AsterClient.connect(List.of("127.0.0.1:7000"));
Collection products = client.collection("products");
products.upsert("doc-1", vector, /* tags… */);
List<Hit> hits = products.search(query, 10, 128);
```

```scala
val client = AsterClient.connect(Seq("127.0.0.1:7000"))
val products = client.collection("products")
products.upsert("doc-1", vector, tags = Set("electronics"))
val hits = products.search(query, topK = 10, efSearch = 128)
```

## Protocol reference

Collections and RPCs are defined in Thrift:

- `createCollection` / `dropCollection`
- `upsert` / `get` / `remove` / `search`

Target product options (accuracy profiles, HOT/WARM/COLD storage, quotas)
are described in [`../client-api.md`](../client-api.md). The Thrift IDL is
the wire source of truth; the collection-config doc is the longer-term
SaaS shape.

## Until the server ships

1. Embed [`aster::Db`](database-management.md) or `aster::embedded::Db`.
2. Keep client call sites against these facades — M5 fills in transport
   without changing the examples above.
3. Track progress in the [development plan](../development-plan.md) (M4 server, M5 clients).

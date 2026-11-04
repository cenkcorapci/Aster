# aster-client (Python)

Python client for Aster over the HTTP JSON API (`aster serve`). Pure stdlib,
no runtime dependencies.

Tutorial: [docs/tutorials/client-libraries.md](../../docs/tutorials/client-libraries.md).

```python
from aster import Client

client = Client(base_url="http://127.0.0.1:8080", api_key=None)   # or seeds=["host:8080"]
assert client.health()

products = client.create_collection("products", 384, metric="cosine")  # idempotent
products.upsert("doc-1", vector, tags=["electronics"], metadata="free text")
hits = products.search(query_vector, top_k=10, ef_search=128, tags=["electronics"])
for hit in hits:
    print(hit.id, hit.score)

doc = products.get("doc-1")        # Document | None
products.delete("doc-1")
print(products.stats())            # GET /v1/collections/{name}/stats
```

Vectors may be any sequence of floats (including NumPy arrays). Errors raise
`AsterError` (`NotFoundError` for 404, `AsterConnectionError` for transport
failures / timeouts).

## Install / test

```bash
pip install ./clients/python            # or: pip wheel clients/python
bazel build //aster/cli:aster
python -m unittest discover -s clients/python/tests -t clients/python
```

`LiveServerTests` spawn `aster serve` from `$ASTER_BIN` or
`bazel-bin/aster/cli/aster` and are skipped when no binary is found.

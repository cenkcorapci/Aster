# aster-client (Python)

Python client for Aster. Facade is ready; transport lands in milestone M5.

Tutorial: [docs/tutorials/client-libraries.md](../../docs/tutorials/client-libraries.md).

```python
from aster import Client

client = Client(seeds=["127.0.0.1:7000"])
products = client.collection("products")

products.upsert("doc-1", vector, tags=["electronics"])
hits = products.search(query_vector, top_k=10, ef_search=128)
for hit in hits:
    print(hit.id, hit.score)
```

Vectors may be any sequence of floats (including NumPy arrays). Published
to PyPI from the monorepo release pipeline (M6) — see
[`clients/README.md`](../README.md).

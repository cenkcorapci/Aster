# aster-client (Python)

Python client for [Aster](https://github.com/aster-db/aster). Published to
PyPI from the monorepo release pipeline (see `clients/README.md`).

```python
from aster import Client

client = Client(seeds=["10.0.0.1:7000"])
products = client.collection("products")

products.upsert("doc-1", vector, tags={"electronics"})
hits = products.search(query_vector, top_k=10, ef_search=128)
for hit in hits:
    print(hit.id, hit.score)
```

Vectors may be any sequence of floats, including numpy arrays.

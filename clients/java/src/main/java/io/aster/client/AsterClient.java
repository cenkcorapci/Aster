package io.aster.client;

import java.util.List;
import java.util.Set;

/**
 * Java client for Aster, the peer-to-peer vector database.
 *
 * <p>Protocol: {@code //aster/rpc/aster.thrift}. Transport and generated
 * code land in milestone M5; this class fixes the public API surface.
 * Published to Maven Central as {@code io.aster:aster-client} from the
 * monorepo release pipeline (see clients/README.md).
 *
 * <pre>{@code
 * try (AsterClient client = AsterClient.connect(List.of("10.0.0.1:7000"))) {
 *   AsterClient.Collection products = client.collection("products");
 *   products.upsert("doc-1", vector, Set.of("electronics"), null);
 *   List<Hit> hits = products.search(query, SearchOptions.topK(10).efSearch(128));
 * }
 * }</pre>
 */
public final class AsterClient implements AutoCloseable {

  /** A single search result; higher score is better for all metrics. */
  public record Hit(String id, double score, byte[] metadata) {}

  /** Per-call search options with collection-level defaults. */
  public static final class SearchOptions {
    private int topK = 10;
    private int efSearch; // 0 = collection default
    private Set<String> tags = Set.of();

    public static SearchOptions topK(int k) {
      SearchOptions o = new SearchOptions();
      o.topK = k;
      return o;
    }

    public SearchOptions efSearch(int ef) {
      this.efSearch = ef;
      return this;
    }

    public SearchOptions tags(Set<String> tags) {
      this.tags = tags;
      return this;
    }
  }

  /** Handle to one collection; cheap to create and thread-safe. */
  public final class Collection {
    private final String name;

    private Collection(String name) {
      this.name = name;
    }

    public void upsert(String id, float[] vector, Set<String> tags, byte[] metadata) {
      throw new UnsupportedOperationException("transport lands in milestone M5");
    }

    public void delete(String id) {
      throw new UnsupportedOperationException("transport lands in milestone M5");
    }

    public List<Hit> search(float[] vector, SearchOptions options) {
      throw new UnsupportedOperationException("transport lands in milestone M5");
    }
  }

  private final List<String> seeds;

  private AsterClient(List<String> seeds) {
    this.seeds = List.copyOf(seeds);
  }

  /** Connects to any node; every Aster node can coordinate requests. */
  public static AsterClient connect(List<String> seeds) {
    return new AsterClient(seeds);
  }

  public Collection collection(String name) {
    return new Collection(name);
  }

  @Override
  public void close() {}
}

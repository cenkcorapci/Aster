/**
 * JavaScript/TypeScript client for Aster, the peer-to-peer vector database.
 *
 * Protocol: `//aster/rpc/aster.thrift`. Transport and generated code land
 * in milestone M5; this module fixes the public API surface. Published to
 * npm as `@aster-db/client` (see clients/README.md).
 *
 * ```ts
 * const client = await AsterClient.connect({ seeds: ["10.0.0.1:7000"] });
 * const products = client.collection("products");
 * await products.upsert("doc-1", vector, { tags: ["electronics"] });
 * const hits = await products.search(query, { topK: 10, efSearch: 128 });
 * ```
 */

export interface ClientOptions {
  seeds: string[];
  tls?: boolean;
  timeoutMs?: number;
}

export interface Hit {
  id: string;
  score: number;
  metadata?: Uint8Array;
}

export interface UpsertOptions {
  tags?: string[];
  metadata?: Uint8Array;
  consistency?: "ONE" | "QUORUM" | "ALL";
}

export interface SearchOptions {
  topK?: number;
  /** Per-query recall/latency knob; omitted = collection default. */
  efSearch?: number;
  tags?: string[];
  consistency?: "ONE" | "QUORUM" | "ALL";
}

/** Vectors are accepted as Float32Array or number[]. */
export type Vector = Float32Array | number[];

export class Collection {
  constructor(readonly name: string) {}

  async upsert(id: string, vector: Vector, options?: UpsertOptions): Promise<void> {
    throw new Error("transport lands in milestone M5");
  }

  async delete(id: string): Promise<void> {
    throw new Error("transport lands in milestone M5");
  }

  async search(vector: Vector, options?: SearchOptions): Promise<Hit[]> {
    throw new Error("transport lands in milestone M5");
  }
}

export class AsterClient {
  private constructor(private readonly options: ClientOptions) {}

  /** Connects to any node; every Aster node can coordinate requests. */
  static async connect(options: ClientOptions): Promise<AsterClient> {
    return new AsterClient(options);
  }

  collection(name: string): Collection {
    return new Collection(name);
  }

  async close(): Promise<void> {}
}

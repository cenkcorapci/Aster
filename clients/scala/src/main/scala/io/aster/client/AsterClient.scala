package io.aster.client

import scala.concurrent.Future

/** Scala client for Aster, the peer-to-peer vector database.
  *
  * Protocol: `//aster/rpc/aster.thrift`. Transport and generated code land
  * in milestone M5; this file fixes the public API surface. Published to
  * Maven Central as `io.aster:aster-client-scala` (see clients/README.md).
  *
  * {{{
  * val client   = AsterClient.connect(Seq("10.0.0.1:7000"))
  * val products = client.collection("products")
  * products.upsert("doc-1", vector, tags = Set("electronics"))
  * val hits: Future[Seq[Hit]] =
  *   products.search(query, SearchOptions(topK = 10, efSearch = Some(128)))
  * }}}
  */
final case class Hit(id: String, score: Double, metadata: Option[Array[Byte]])

final case class SearchOptions(
    topK: Int = 10,
    efSearch: Option[Int] = None, // None = collection default
    tags: Set[String] = Set.empty
)

final class Collection private[client] (val name: String) {

  def upsert(
      id: String,
      vector: Array[Float],
      tags: Set[String] = Set.empty,
      metadata: Option[Array[Byte]] = None
  ): Future[Unit] =
    Future.failed(new NotImplementedError("transport lands in milestone M5"))

  def delete(id: String): Future[Unit] =
    Future.failed(new NotImplementedError("transport lands in milestone M5"))

  def search(vector: Array[Float], options: SearchOptions = SearchOptions()): Future[Seq[Hit]] =
    Future.failed(new NotImplementedError("transport lands in milestone M5"))
}

final class AsterClient private (seeds: Seq[String]) {
  def collection(name: String): Collection = new Collection(name)
  def close(): Unit = ()
}

object AsterClient {

  /** Connects to any node; every Aster node can coordinate requests. */
  def connect(seeds: Seq[String]): AsterClient = new AsterClient(seeds)
}

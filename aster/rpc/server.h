#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "aster/core/status.h"
#include "aster/rpc/handler.h"

namespace apache {
namespace thrift {
namespace server {
class TServer;
}  // namespace server
namespace transport {
class TServerSocket;
}  // namespace transport
}  // namespace thrift
}  // namespace apache

namespace aster {
namespace rpc {

// Framed-TCP Thrift server for the Aster service (TBinaryProtocol +
// TFramedTransport + TThreadedServer).
class ThriftServer {
 public:
  struct Options {
    std::string host = "127.0.0.1";
    uint16_t port = 9090;

    // Optional TLS. When enabled, the server accepts both plaintext and TLS
    // connections on the same port (TLS record sniffing).
    bool tls = false;
    bool tls_insecure = true;  // no client-certificate verification
    std::string tls_cert_file;
    std::string tls_key_file;
    std::string tls_ca_file;  // optional (only relevant when tls_insecure=false)
  };

  ThriftServer(Options options, std::shared_ptr<AsterHandler> handler);

  // Bind and listen. Port 0 selects an ephemeral port; use port() after.
  Status Listen();
  // Blocking serve loop (run on a dedicated thread in tests).
  void Serve();
  void Stop();

  uint16_t port() const { return bound_port_; }

 private:
  Options options_;
  std::shared_ptr<AsterHandler> handler_;
  std::shared_ptr<::apache::thrift::transport::TServerSocket> server_socket_;
  std::shared_ptr<::apache::thrift::server::TServer> server_;
  uint16_t bound_port_ = 0;
};

}  // namespace rpc
}  // namespace aster

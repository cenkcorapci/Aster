#include "aster/rpc/server.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <memory>
#include <stdexcept>

#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/server/TThreadedServer.h>
#include <thrift/transport/TBufferTransports.h>
#include <thrift/transport/TServerSocket.h>
#include <thrift/transport/TSSLSocket.h>
#include <thrift/transport/TTransportException.h>

#include "Aster.h"
#include "aster/rpc/tls_maybe_server_socket.h"

namespace aster {
namespace rpc {

namespace atp = ::apache::thrift::protocol;
namespace ats = ::apache::thrift::server;
namespace att = ::apache::thrift::transport;
namespace atc = ::apache::thrift::concurrency;

ThriftServer::ThriftServer(Options options,
                           std::shared_ptr<AsterHandler> handler)
    : options_(std::move(options)), handler_(std::move(handler)) {}

Status ThriftServer::Listen() {
  if (!handler_) {
    return Status::InvalidArgument("ThriftServer requires a handler");
  }

  // Bind ourselves (supports port 0) then hand the fd to Thrift so
  // TServerFramework::serve() can call listen() without rebinding.
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return Status::IoError("socket failed");
  int yes = 1;
  ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  sockaddr_in addr {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(options_.port);
  if (::inet_pton(AF_INET, options_.host.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    return Status::InvalidArgument("bad host");
  }
  if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    return Status::IoError("bind failed");
  }
  if (::listen(fd, 64) < 0) {
    ::close(fd);
    return Status::IoError("listen failed");
  }

  sockaddr_in bound {};
  socklen_t len = sizeof(bound);
  if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &len) == 0) {
    bound_port_ = ntohs(bound.sin_port);
  } else {
    bound_port_ = options_.port;
  }
  if (bound_port_ == 0) {
    ::close(fd);
    return Status::IoError("thrift listen: invalid bound port");
  }

  try {
    if (options_.tls) {
      if (options_.tls_cert_file.empty() || options_.tls_key_file.empty()) {
        ::close(fd);
        return Status::InvalidArgument(
            "thrift listen: --tls requires --tls-cert and --tls-key");
      }

      auto tls_factory =
          std::make_shared<att::TSSLSocketFactory>(att::SSLProtocol::LATEST);
      // Server-side handshake.
      tls_factory->server(true);
      tls_factory->loadCertificate(options_.tls_cert_file.c_str(), "PEM");
      tls_factory->loadPrivateKey(options_.tls_key_file.c_str(), "PEM");
      if (!options_.tls_ca_file.empty()) {
        // Client auth / verification trust store.
        tls_factory->loadTrustedCertificates(options_.tls_ca_file.c_str(),
                                              nullptr);
      }
      // In insecure mode we do not require or verify client certificates.
      tls_factory->authenticate(!options_.tls_insecure);

      server_socket_ = std::make_shared<MaybeTLSServerSocket>(
          fd, att::SocketType::INET, tls_factory);
    } else {
      server_socket_ = std::make_shared<att::TServerSocket>(
          fd, att::SocketType::INET);
    }
    auto transport_factory =
        std::make_shared<att::TFramedTransportFactory>();
    auto protocol_factory = std::make_shared<atp::TBinaryProtocolFactory>();
    auto processor = std::make_shared<AsterProcessor>(handler_);
    auto thread_factory = std::make_shared<atc::ThreadFactory>();

    server_ = std::make_shared<ats::TThreadedServer>(
        processor, server_socket_, transport_factory, protocol_factory,
        thread_factory);
    return Status::Ok();
  } catch (const att::TTransportException& ex) {
    ::close(fd);
    return Status::IoError(std::string("thrift setup failed: ") + ex.what());
  } catch (const std::exception& ex) {
    ::close(fd);
    return Status::IoError(std::string("thrift setup failed: ") + ex.what());
  }
}

void ThriftServer::Serve() {
  if (!server_) return;
  server_->serve();
}

void ThriftServer::Stop() {
  if (server_) {
    server_->stop();
  }
}

}  // namespace rpc
}  // namespace aster

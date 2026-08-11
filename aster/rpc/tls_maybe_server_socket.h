#pragma once

#include <memory>
#include <utility>

#include <sys/socket.h>

#include <thrift/transport/TServerSocket.h>
#include <thrift/transport/TSSLSocket.h>

namespace aster {
namespace rpc {

// A Thrift TServerSocket that accepts either plaintext (TSocket) or TLS
// (TSSLSocket) on the same listening socket.
//
// This keeps "insecure" (non-TLS) clients working even when TLS mode is
// enabled for the RPC server.
class MaybeTLSServerSocket : public ::apache::thrift::transport::TServerSocket {
 public:
  MaybeTLSServerSocket(
      THRIFT_SOCKET sock,
      ::apache::thrift::transport::SocketType socket_type,
      std::shared_ptr<::apache::thrift::transport::TSSLSocketFactory> tls_factory)
      : ::apache::thrift::transport::TServerSocket(sock, socket_type),
        tls_factory_(std::move(tls_factory)) {
    tls_factory_->server(true);
  }

 protected:
  std::shared_ptr<::apache::thrift::transport::TSocket> createSocket(
      THRIFT_SOCKET client) override {
    // Peek for TLS record header:
    //   ContentType (0x16 for Handshake), VersionMajor (0x03), VersionMinor.
    unsigned char hdr[3] = {0, 0, 0};
    const ssize_t n = ::recv(client, hdr, sizeof(hdr), MSG_PEEK | MSG_NOSIGNAL);
    const bool looks_like_tls =
        n >= 3 && hdr[0] == 0x16 && hdr[1] == 0x03;

    if (looks_like_tls) {
      if (interruptableChildren_) {
        return tls_factory_->createSocket(client, pChildInterruptSockReader_);
      }
      return tls_factory_->createSocket(client);
    }

    // Fall back to the default (plaintext) socket type.
    return ::apache::thrift::transport::TServerSocket::createSocket(client);
  }

 private:
  std::shared_ptr<::apache::thrift::transport::TSSLSocketFactory> tls_factory_;
};

}  // namespace rpc
}  // namespace aster


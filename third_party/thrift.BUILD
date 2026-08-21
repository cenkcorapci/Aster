# Minimal Apache Thrift C++ runtime for Aster RPC stubs + framed TCP server.
#
# Builds the subset needed for generated types, service interfaces, and a
# framed-TCP TThreadedServer (binary protocol, buffer/socket transports).
# TLS/SSL is optional in Aster (M4-T03). We include the necessary TSSL*
# transport pieces so `serve-rpc --tls ...` can run.

load("@rules_cc//cc:cc_library.bzl", "cc_library")

package(default_visibility = ["//visibility:public"])

licenses(["notice"])  # Apache 2.0

exports_files(["LICENSE"])

# Written by patch_cmds in MODULE.bazel (http_archive of apache/thrift).
_CONFIG_H = "lib/cpp/src/thrift/config.h"

cc_library(
    name = "thrift",
    srcs = [
        "lib/cpp/src/thrift/TApplicationException.cpp",
        "lib/cpp/src/thrift/TOutput.cpp",
        "lib/cpp/src/thrift/TUuid.cpp",
        "lib/cpp/src/thrift/async/TConcurrentClientSyncInfo.cpp",
        "lib/cpp/src/thrift/concurrency/Monitor.cpp",
        "lib/cpp/src/thrift/concurrency/Mutex.cpp",
        "lib/cpp/src/thrift/concurrency/Thread.cpp",
        "lib/cpp/src/thrift/concurrency/ThreadFactory.cpp",
        "lib/cpp/src/thrift/protocol/TProtocol.cpp",
        "lib/cpp/src/thrift/server/TConnectedClient.cpp",
        "lib/cpp/src/thrift/server/TServer.cpp",
        "lib/cpp/src/thrift/server/TServerFramework.cpp",
        "lib/cpp/src/thrift/server/TSimpleServer.cpp",
        "lib/cpp/src/thrift/server/TThreadedServer.cpp",
        "lib/cpp/src/thrift/transport/SocketCommon.cpp",
        "lib/cpp/src/thrift/transport/TBufferTransports.cpp",
        "lib/cpp/src/thrift/transport/TNonblockingServerSocket.cpp",
        "lib/cpp/src/thrift/transport/TServerSocket.cpp",
        "lib/cpp/src/thrift/transport/TSocket.cpp",
        "lib/cpp/src/thrift/transport/TTransportException.cpp",
    ] + glob(
        [
            "lib/cpp/src/thrift/transport/TSSL*.cpp",
            "lib/cpp/src/thrift/transport/TNonblockingSSL*.cpp",
        ],
    ) + glob(
        [
            "lib/cpp/src/thrift/*.h",
            "lib/cpp/src/thrift/async/*.h",
            "lib/cpp/src/thrift/concurrency/*.h",
            "lib/cpp/src/thrift/protocol/*.h",
            "lib/cpp/src/thrift/protocol/*.tcc",
            "lib/cpp/src/thrift/server/*.h",
            "lib/cpp/src/thrift/transport/*.h",
            "lib/cpp/src/thrift/processor/*.h",
        ],
        exclude = [
            "lib/cpp/src/thrift/transport/THttp*.h",
            "lib/cpp/src/thrift/transport/THeaderTransport.h",
            "lib/cpp/src/thrift/transport/TZlibTransport.h",
            "lib/cpp/src/thrift/transport/TWebSocketServer.h",
            "lib/cpp/src/thrift/protocol/TJSONProtocol.h",
            "lib/cpp/src/thrift/protocol/THeaderProtocol.h",
            "lib/cpp/src/thrift/processor/TMultiplexedProcessor.h",
            "lib/cpp/src/thrift/server/TNonblockingServer.h",
            "lib/cpp/src/thrift/server/TThreadPoolServer.h",
        ],
    ),
    hdrs = [
        _CONFIG_H,
        "compiler/cpp/src/thrift/version.h",
        "lib/cpp/src/boost/shared_array.hpp",
    ] + glob(
        [
            "openssl/include/openssl/**/*.h",
            "openssl/include/openssl/**/*.hpp",
        ],
        allow_empty = True,
    ),
    copts = [
        "-Wno-unused-parameter",
        "-Wno-unused-const-variable",
    ],
    defines = [
        "THRIFT_STATIC_DEFINE",
    ],
    includes = [
        "lib/cpp/src",
        "openssl/include",
    ],
    linkopts = select({
        "@platforms//os:windows": [],
        "@platforms//os:macos": [
            "-lpthread",
            "-L/opt/homebrew/lib",
            "-lssl",
            "-lcrypto",
        ],
        "//conditions:default": [
            "-lpthread",
            "-lssl",
            "-lcrypto",
        ],
    }),
    textual_hdrs = [
        "lib/cpp/src/thrift/protocol/TBinaryProtocol.tcc",
        "lib/cpp/src/thrift/protocol/TCompactProtocol.tcc",
    ],
)

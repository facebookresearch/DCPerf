/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/init/Init.h>
#include <folly/logging/xlog.h>
#include <folly/portability/GFlags.h>

#include "proxygen/lib/http/coro/server/HTTPServer.h"
#include "ti/foss_revproxy/server/ContentHandler.h"

using namespace proxygen::coro;
using namespace ti::foss_revproxy;

// Configuration flags
DEFINE_int32(port, 8082, "Port to listen on");
DEFINE_string(cert, "", "TLS certificate file (optional)");
DEFINE_string(key, "", "TLS key file (optional)");
DEFINE_bool(quic, false, "Enable QUIC/HTTP3 (requires cert/key)");
DEFINE_string(plaintext_proto, "", "Plaintext protocol (h2 or http/1.1)");
DEFINE_double(
    reset_probability,
    0.0,
    "Probability (0.0-1.0) of randomly resetting connections");

int main(int argc, char** argv) {
  const folly::Init init(&argc, &argv);
  ::gflags::ParseCommandLineFlags(&argc, &argv, false);

  XLOG(INFO) << "=== FOSS Revproxy Content Server ===";
  XLOG(INFO) << "Listening on port " << FLAGS_port;
  XLOG(INFO) << "Reset probability: " << FLAGS_reset_probability;

  // Create HTTP server configuration
  HTTPServer::Config config;
  config.socketConfig.bindAddress.setFromLocalPort(FLAGS_port);
  config.plaintextProtocol = FLAGS_plaintext_proto;

  // Configure TLS if provided
  if (!FLAGS_cert.empty() && !FLAGS_key.empty()) {
    XLOG(INFO) << "TLS enabled";
    if (FLAGS_quic) {
      XLOG(INFO) << "QUIC/HTTP3 enabled";
    }

    auto tlsConfig = HTTPServer::getDefaultTLSConfig();
    try {
      tlsConfig.setCertificate(FLAGS_cert, FLAGS_key, "");
    } catch (const std::exception& ex) {
      XLOG(ERR) << "Failed to load TLS certificate: " << ex.what();
      return 1;
    }

    config.socketConfig.sslContextConfigs.emplace_back(std::move(tlsConfig));

    if (FLAGS_quic) {
      XLOG(INFO) << "Enabling QUIC/HTTP3 support";
      config.quicConfig = HTTPServer::QuicConfig();
    }
  } else if (FLAGS_quic) {
    XLOG(ERR) << "QUIC requires --cert and --key";
    return 1;
  } else {
    XLOG(INFO) << "Running in plaintext mode";
  }

  // Create content handler
  auto handler = std::make_shared<ContentHandler>(FLAGS_reset_probability);

  XLOG(INFO) << "Starting HTTP server...";
  XLOG(INFO) << "Content types: HTML, JavaScript, JSON, PNG images";

  // Create and start server
  HTTPServer server(std::move(config), handler);
  server.start();

  XLOG(INFO) << "=== Content Server Shutdown ===";
  return 0;
}

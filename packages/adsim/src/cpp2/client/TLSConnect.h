/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <fizz/client/AsyncFizzClient.h>
#include <folly/futures/Promise.h>
#include <folly/io/async/EventBase.h>
#include <thrift/lib/cpp2/security/extensions/ThriftParametersClientExtension.h>

namespace facebook {
namespace cea {
namespace chips {
namespace adsim {

/* A helper class to create fizz sockets.
 * From fbcode/thrift/lib/cpp2/test/server/ThriftServerTest.cpp
 */
class FizzStopTLSConnector
    : public fizz::client::AsyncFizzClient::HandshakeCallback,
      public fizz::AsyncFizzBase::EndOfTLSCallback {
 public:
  folly::AsyncSocket::UniquePtr connect(
      const folly::SocketAddress& addr,
      folly::EventBase* evb) {
    eb_ = evb;

    auto sock = folly::AsyncSocket::newSocket(&handshake_eb_, addr);
    auto ctx = std::make_shared<fizz::client::FizzClientContext>();
    ctx->setSupportedAlpns({"rs"});
    auto thriftParametersContext =
        std::make_shared<apache::thrift::ThriftParametersContext>();
    thriftParametersContext->setUseStopTLS(true);
    auto extension =
        std::make_shared<apache::thrift::ThriftParametersClientExtension>(
            thriftParametersContext);

    client_.reset(new fizz::client::AsyncFizzClient(
        std::move(sock), std::move(ctx), std::move(extension)));
    client_->connect(
        this,
        nullptr,
        folly::none,
        folly::none,
        folly::Optional<std::vector<fizz::ech::ParsedECHConfig>>(folly::none),
        std::chrono::milliseconds(3000));
    return promise_.getFuture().getVia(&handshake_eb_);
  }

  void fizzHandshakeSuccess(
      fizz::client::AsyncFizzClient* client) noexcept override {
    VLOG(2) << "fizzHandShakeSuccess";
    client->setEndOfTLSCallback(this);
  }

  void fizzHandshakeError(
      fizz::client::AsyncFizzClient* /* unused */,
      folly::exception_wrapper ex) noexcept override {
    promise_.setException(ex);
    VLOG(2) << "Fizz handshake failed " << ex.what();
  }

  void endOfTLS(fizz::AsyncFizzBase* transport, std::unique_ptr<folly::IOBuf>)
      override {
    auto sock = transport->getUnderlyingTransport<folly::AsyncSocket>();
    DCHECK(sock);

    auto fd = sock->detachNetworkSocket();
    auto zcId = sock->getZeroCopyBufId();

    // create new socket
    auto plaintextTransport =
        folly::AsyncSocket::UniquePtr(new folly::AsyncSocket(eb_, fd, zcId));
    promise_.setValue(std::move(plaintextTransport));
  }

  fizz::client::AsyncFizzClient::UniquePtr client_;
  folly::Promise<folly::AsyncSocket::UniquePtr> promise_;
  folly::EventBase* eb_;
  folly::EventBase handshake_eb_;
};

} // namespace adsim
} // namespace chips
} // namespace cea
} // namespace facebook

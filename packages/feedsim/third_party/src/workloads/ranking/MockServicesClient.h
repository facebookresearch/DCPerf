// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

#include <folly/SocketAddress.h>
#include <folly/futures/Future.h>
#include <folly/io/async/AsyncTimeout.h>
#include <folly/io/async/EventBase.h>

#include "RpcDistRegistry.h"

// The generated cpp2 client is exposed as a `using` alias
// (`mock_services::MockServiceAsyncClient = apache::thrift::Client<...>`),
// so a `class` forward declaration is incompatible. Pull in the generated
// header directly. Consumers of this header therefore depend transitively
// on the MockService-cpp2 thrift target (the LeafNodeRank target already
// does via add_dependencies(LeafNodeRank MockService-cpp2-target)).
#include "mock_services/gen-cpp2/MockServiceAsyncClient.h"

namespace ranking {

/**
 * MockServicesClient — per-thread wrapper around the generated
 * MockServiceAsyncClient. Each LeafNodeRank worker thread owns one
 * instance, pinned to its own folly::EventBase (typically a thread from
 * the SREventBase pool — matches the production EventBase placement).
 *
 * Thread-safety: NOT thread-safe. Each instance is tied to one EventBase
 * and must only be used from that EventBase's thread (or via futures
 * dispatched onto it). dispatchByEnum() takes the request payload as
 * std::string (moved) and returns a SemiFuture that completes with the
 * server's response payload.
 *
 * Wire contract: the request payload must be at least 4 bytes long,
 * with the first 4 bytes encoding response_size as a big-endian
 * uint32_t (see mock_services/MockService.thrift). The mock server
 * uses this header to size its response.
 */
class MockServicesClient {
 public:
  /**
   * Connect to the mock_services Thrift server at host:port using a
   * RocketClientChannel pinned to `evb`. `evb` must outlive this client
   * (typically a thread from a long-lived IOThreadPoolExecutor).
   *
   * Throws on connect failure (the rtptest deployment expects the mock
   * server to be up before LeafNodeRank starts; failing fast surfaces
   * the misconfiguration immediately rather than producing silent
   * fallback behavior at request time).
   */
  MockServicesClient(
      folly::EventBase* evb,
      const std::string& host,
      uint16_t port,
      std::chrono::milliseconds keepalive_interval =
          std::chrono::milliseconds(0));

  ~MockServicesClient();

  // Non-copyable, non-movable (holds a thread-affine AsyncClient).
  MockServicesClient(const MockServicesClient&) = delete;
  MockServicesClient& operator=(const MockServicesClient&) = delete;
  MockServicesClient(MockServicesClient&&) = delete;
  MockServicesClient& operator=(MockServicesClient&&) = delete;

  /**
   * Dispatch one of the 20 mock RPCs by enum index. Compile-time switch
   * over MethodIdx so each call invokes the named generated
   * semifuture_<method>() — preserving distinct Strobelight symbols per
   * RPC type, which is the entire reason we have 20 thrift methods.
   *
   * `request` must include the 4-byte big-endian response_size header.
   * `latency_us` is the simulated server-side latency in microseconds.
   *
   * Returns a SemiFuture that completes with the server's response body
   * (binary -> std::string in the generated cpp2 client), or fails on
   * transport error / RPC exception.
   */
  folly::SemiFuture<std::string> dispatchByEnum(
      MethodIdx idx,
      const std::string& request,
      int32_t latency_us);

  folly::EventBase* getEventBase() const { return evb_; }

 private:
  // KeepaliveTimer (defined in .cc) fires a fire-and-forget getStatus()
  // probe RPC every keepalive_interval_ms to keep the underlying Rocket
  // channel + SREventBase warm. Forward declared here so the unique_ptr
  // member doesn't pull AsyncTimeout into the header's public surface.
  class KeepaliveTimer;

  folly::EventBase* evb_;
  std::unique_ptr<mock_services::MockServiceAsyncClient> client_;
  std::unique_ptr<KeepaliveTimer> keepalive_;
};

/**
 * Encode a 4-byte big-endian response_size header at the start of `dst`.
 * Caller must ensure `dst` has at least 4 bytes capacity. This is the
 * wire contract documented in mock_services/MockService.thrift.
 */
inline void writeBigEndianResponseSize(char* dst, uint32_t response_size) {
  dst[0] = static_cast<char>((response_size >> 24) & 0xFF);
  dst[1] = static_cast<char>((response_size >> 16) & 0xFF);
  dst[2] = static_cast<char>((response_size >> 8) & 0xFF);
  dst[3] = static_cast<char>(response_size & 0xFF);
}

} // namespace ranking

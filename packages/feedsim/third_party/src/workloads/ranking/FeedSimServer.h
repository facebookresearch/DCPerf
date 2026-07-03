// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace feedsim {

class RequestContext;

// Callback types — simplified from oldisim's Callbacks.h
// thread_id replaces oldisim::NodeThread (we only ever used get_thread_num())
using QueryCallback = std::function<void(int thread_id, RequestContext& ctx)>;
using ThreadStartupCallback = std::function<void(int thread_id)>;

/**
 * RequestContext replaces oldisim::QueryContext.
 *
 * Key differences from oldisim::QueryContext:
 * - No friend classes or private constructors
 * - sendResponse() internally handles wire framing
 * - Move constructor works cleanly for async handler patterns
 */
class RequestContext {
 public:
  RequestContext(RequestContext&& other) noexcept;
  RequestContext(const RequestContext&) = delete;
  RequestContext& operator=(const RequestContext&) = delete;
  ~RequestContext();

  const uint32_t type;
  const uint64_t request_id;
  const uint64_t start_time;
  const uint32_t payload_length;
  const void* payload;

  void sendResponse(const void* data, uint32_t data_length);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  RequestContext(uint32_t type, uint64_t request_id, uint64_t start_time,
                 uint32_t payload_length, const void* payload,
                 std::unique_ptr<Impl> impl);

  friend class ServerConnection;
};

/**
 * FeedSimServer replaces oldisim::LeafNodeServer.
 *
 * Same public API shape. Internally uses:
 * - POSIX sockets + epoll (via folly::EventBase) instead of raw libevent
 * - folly::MPMCQueue instead of boost::lockfree::queue for work stealing
 * - std::thread + sched_setaffinity instead of raw pthread
 * - folly::dynamic + folly::toJson instead of cereal for monitoring JSON
 *
 * Wire protocol is identical to oldisim (QueryPacketHeader/ResponsePacketHeader).
 */
class FeedSimServer {
 public:
  explicit FeedSimServer(uint16_t port);
  ~FeedSimServer();

  void setNumThreads(uint32_t num_threads);
  void setThreadPinning(bool enabled);
  void setThreadLoadBalancing(bool enabled);
  void setThreadStartupCallback(ThreadStartupCallback cb);
  void registerQueryCallback(uint32_t type, QueryCallback cb);
  void enableMonitoring(uint16_t port);

  void run();      // Blocks until shutdown
  void shutdown();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace feedsim

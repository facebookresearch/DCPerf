// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <folly/Expected.h>
#include <folly/Synchronized.h>
#include <folly/fibers/FiberManager.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>
#include <chrono>
#include <functional>
#include <unordered_set>

namespace facebook::ucachebench {

/**
 * UcacheBenchIOThreadContext maintains thread-local states to track load status
 * for each IO-threads.
 * To use, init() must be called from each IO-thread.
 */
class UcacheBenchIOThreadContext {
 public:
  /**
   * @return Thread-local instance. init() must be called
   * prior to this for each IO-thread.
   */
  static UcacheBenchIOThreadContext& tlInstance() {
    XCHECK(tlInstanceImpl())
        << "UcacheBenchIOThreadContext is not initialized (for this thread). Call init().";
    return *tlInstanceImpl();
  }

  /**
   * Creates and initializes thread local instance of UcacheBenchIOThreadContext
   */
  static void init(folly::EventBase& evb);

  /**
   * Returns true if the TL instance of UcacheBenchIOThreadContext is
   * initialized for the calling thread;
   */
  static bool isInitializedForCurrentThread();

  /**
   * Returns EventBase associated with this thread-local instance.
   */
  folly::EventBase& getEventBase() {
    return evb_;
  }
  const folly::EventBase& getEventBase() const {
    return evb_;
  }

  /**
   * The fiber manager that this thread is looping.
   */
  folly::fibers::FiberManager& fm() {
    return *fm_;
  }
  const folly::fibers::FiberManager& fm() const {
    return *fm_;
  }

  /**
   * Iterates over all active IO thread contexts and calls the
   * callback for each.
   */
  static void forEachContext(
      const std::function<void(int, const UcacheBenchIOThreadContext&)>& cb);

 private:
  static std::unique_ptr<UcacheBenchIOThreadContext>& tlInstanceImpl();
  // force use of TLSingleton
  explicit UcacheBenchIOThreadContext(folly::EventBase& evb);

  folly::EventBase& evb_;
  std::unique_ptr<folly::fibers::FiberManager> fm_;
};

} // namespace facebook::ucachebench

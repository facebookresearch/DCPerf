// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "UcacheBenchIOThreadContext.h"

#include <folly/fibers/EventBaseLoopController.h>

#include <folly/portability/GFlags.h>
#include <chrono>

DEFINE_uint32(
    fiber_stack_size,
    64 * 1024,
    "Stack size for IO thread fibers in bytes");

DEFINE_uint32(fiber_max_pool_size, 1000, "Maximum size of the fiber pool");

DEFINE_uint32(
    fiber_pool_resize_period_ms,
    1000,
    "Period in ms for resizing the fiber pool");

DEFINE_bool(
    enable_fibers,
    false,
    "Enable fiber-based request processing. "
    "Disabled by default to match production ucache behavior.");

namespace facebook::ucachebench {

namespace {

std::unique_ptr<folly::fibers::EventBaseLoopController> makeLoopController(
    folly::EventBase& base) {
  auto loopController =
      std::make_unique<folly::fibers::EventBaseLoopController>();
  loopController->attachEventBase(base);
  return loopController;
}

folly::fibers::FiberManager::Options makeFmOptions() {
  folly::fibers::FiberManager::Options opts;
  opts.stackSize = FLAGS_fiber_stack_size;
  opts.maxFibersPoolSize = FLAGS_fiber_max_pool_size;
  opts.fibersPoolResizePeriodMs = FLAGS_fiber_pool_resize_period_ms;
  return opts;
}

static auto& allUcacheBenchContexts() {
  static folly::Synchronized<std::vector<UcacheBenchIOThreadContext*>>
      ucacheBenchContexts;
  return ucacheBenchContexts;
}

} // namespace

std::unique_ptr<UcacheBenchIOThreadContext>&
UcacheBenchIOThreadContext::tlInstanceImpl() {
  thread_local std::unique_ptr<UcacheBenchIOThreadContext> impl;
  return impl;
}

void UcacheBenchIOThreadContext::init(folly::EventBase& evb) {
  // Allow re-initialization if already exists
  if (tlInstanceImpl()) {
    return; // Already initialized
  }
  std::unique_ptr<UcacheBenchIOThreadContext> threadCtx(
      new UcacheBenchIOThreadContext(evb));
  tlInstanceImpl() = std::move(threadCtx);
  allUcacheBenchContexts().wlock()->push_back(tlInstanceImpl().get());
}

void UcacheBenchIOThreadContext::forEachContext(
    const std::function<void(int, const UcacheBenchIOThreadContext&)>& cb) {
  auto lockPtr = allUcacheBenchContexts().rlock();
  for (int i = 0; i < lockPtr->size(); i++) {
    if (cb) {
      cb(i, *(*lockPtr)[i]);
    }
  }
}

UcacheBenchIOThreadContext::UcacheBenchIOThreadContext(folly::EventBase& evb)
    : evb_(evb),
      fm_(std::make_unique<folly::fibers::FiberManager>(
          makeLoopController(evb_),
          makeFmOptions())) {}

bool UcacheBenchIOThreadContext::isInitializedForCurrentThread() {
  return tlInstanceImpl().get() != nullptr;
}

} // namespace facebook::ucachebench

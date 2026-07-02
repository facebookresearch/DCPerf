// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <fmt/format.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/portability/GFlags.h>
#include <mcrouter/lib/carbon/Result.h>
#include "UcacheBenchIOThreadContext.h"

DECLARE_bool(enable_fibers);
DECLARE_uint32(rpc_num_cpu_worker_threads);

namespace facebook::ucachebench {

// Shared CPU thread pool for IO→CPU→IO dispatch pattern.
// When rpc_num_cpu_worker_threads > 1, requests are dispatched from the IO
// thread to this pool, generating real kernel context switches (futex) that
// match production's thread scheduling patterns.
inline folly::CPUThreadPoolExecutor* getCpuPool() {
  static std::unique_ptr<folly::CPUThreadPoolExecutor> pool;
  static std::once_flag flag;
  std::call_once(flag, [] {
    if (FLAGS_rpc_num_cpu_worker_threads > 1) {
      pool = std::make_unique<folly::CPUThreadPoolExecutor>(
          FLAGS_rpc_num_cpu_worker_threads);
    }
  });
  return pool.get();
}

/**
 * Common entry point to run request with fiber management
 */
template <class Callback, class Request, class Handler>
void ucacheBenchOnRequestCommon(
    Callback&& callback,
    Request&& request,
    Handler&& handler)
  requires(!std::is_reference_v<Callback> && !std::is_reference_v<Request>)
{
  auto* cpuPool = getCpuPool();
  if (cpuPool) {
    cpuPool->add([handler,
                  cb = std::forward<Callback>(callback),
                  req = std::forward<Request>(request)]() mutable {
      handler(std::move(cb), std::move(req));
    });
    return;
  }

  // If fibers are disabled, execute directly
  if (!FLAGS_enable_fibers ||
      !UcacheBenchIOThreadContext::isInitializedForCurrentThread()) {
    handler(std::forward<Callback>(callback), std::forward<Request>(request));
    return;
  }

  // Execute the handler in a fiber
  UcacheBenchIOThreadContext::tlInstance().fm().addTaskEager(
      [handler,
       callbackFiber = std::forward<Callback>(callback),
       requestFiber = std::forward<Request>(request)]() mutable {
        handler(std::move(callbackFiber), std::move(requestFiber));
      });
}

} // namespace facebook::ucachebench

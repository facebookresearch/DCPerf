// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <fmt/format.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/io/async/EventBase.h>
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
 * Common entry point to run a request.
 *
 * `handler` COMPUTES and returns the reply (it must not complete the callback
 * itself). This function owns delivering the reply via `callback->result(...)`.
 * That split matters for the IO→CPU→IO path: these are async_eb handlers, so
 * the callback MUST be completed on its own EventBase. When work is offloaded
 * to the CPU pool we compute the reply there, then bounce the completion back
 * to the callback's EventBase via runInEventBaseThread(). Completing an
 * async_eb callback directly from a CPU-pool thread never delivers the reply —
 * in-flight requests never drain and the server goes idle.
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
    auto* evb = callback->getEventBase();
    cpuPool->add([handler,
                  cb = std::forward<Callback>(callback),
                  req = std::forward<Request>(request),
                  evb]() mutable {
      auto reply = handler(req);
      evb->runInEventBaseThread(
          [cb = std::move(cb), reply = std::move(reply)]() mutable {
            cb->result(std::move(reply));
          });
    });
    return;
  }

  // If fibers are disabled, execute directly on the IO thread.
  if (!FLAGS_enable_fibers ||
      !UcacheBenchIOThreadContext::isInitializedForCurrentThread()) {
    callback->result(handler(request));
    return;
  }

  // Execute the handler in a fiber (so simulateIOLatency can yield).
  UcacheBenchIOThreadContext::tlInstance().fm().addTaskEager(
      [handler,
       cb = std::forward<Callback>(callback),
       req = std::forward<Request>(request)]() mutable {
        cb->result(handler(req));
      });
}

} // namespace facebook::ucachebench

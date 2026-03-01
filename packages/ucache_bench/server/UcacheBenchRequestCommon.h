// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <fmt/format.h>
#include <folly/portability/GFlags.h>
#include <mcrouter/lib/carbon/Result.h>
#include "UcacheBenchIOThreadContext.h"

DECLARE_bool(enable_fibers);

namespace facebook::ucachebench {

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

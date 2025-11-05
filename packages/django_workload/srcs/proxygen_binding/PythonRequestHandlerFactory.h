/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Memory.h>
#include <proxygen/httpserver/RequestHandlerFactory.h>
#include "PythonRequestHandler.h"

namespace ProxygenBinding {

/**
 * Factory for creating PythonRequestHandler instances
 *
 * Supports both synchronous and asynchronous callback modes.
 */
class PythonRequestHandlerFactory : public proxygen::RequestHandlerFactory {
 public:
  // Constructor for synchronous callback (deprecated)
  explicit PythonRequestHandlerFactory(PythonRequestCallback callback);

  // Constructor for asynchronous callback (recommended)
  explicit PythonRequestHandlerFactory(
      PythonAsyncRequestCallback async_callback,
      bool is_async);

  void onServerStart(folly::EventBase* evb) noexcept override;

  void onServerStop() noexcept override;

  proxygen::RequestHandler* onRequest(
      proxygen::RequestHandler* handler,
      proxygen::HTTPMessage* message) noexcept override;

 private:
  PythonRequestCallback sync_callback_;
  PythonAsyncRequestCallback async_callback_;
  bool is_async_;
};

} // namespace ProxygenBinding

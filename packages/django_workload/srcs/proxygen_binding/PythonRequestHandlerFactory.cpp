/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PythonRequestHandlerFactory.h"
#include <glog/logging.h>

namespace ProxygenBinding {

// Constructor for synchronous callback (deprecated)
PythonRequestHandlerFactory::PythonRequestHandlerFactory(
    PythonRequestCallback callback)
    : sync_callback_(std::move(callback)),
      async_callback_(nullptr),
      is_async_(false) {}

// Constructor for asynchronous callback (recommended)
PythonRequestHandlerFactory::PythonRequestHandlerFactory(
    PythonAsyncRequestCallback async_callback,
    bool is_async)
    : sync_callback_(nullptr),
      async_callback_(std::move(async_callback)),
      is_async_(is_async) {}

void PythonRequestHandlerFactory::onServerStart(
    folly::EventBase* /*evb*/) noexcept {
  if (is_async_) {
    LOG(INFO) << "PythonRequestHandlerFactory: Server started (async mode)";
  } else {
    LOG(INFO) << "PythonRequestHandlerFactory: Server started (sync mode)";
  }
}

void PythonRequestHandlerFactory::onServerStop() noexcept {
  LOG(INFO) << "PythonRequestHandlerFactory: Server stopped";
}

proxygen::RequestHandler* PythonRequestHandlerFactory::onRequest(
    proxygen::RequestHandler* /*handler*/,
    proxygen::HTTPMessage* /*msg*/) noexcept {
  if (is_async_) {
    // Create handler with async callback
    return new PythonRequestHandler(async_callback_, true);
  } else {
    // Create handler with sync callback
    return new PythonRequestHandler(sync_callback_);
  }
}

} // namespace ProxygenBinding

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/Memory.h>
#include <folly/io/async/EventBaseManager.h>
#include <proxygen/httpserver/RequestHandler.h>
#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <string>
#include <unordered_map>

namespace py = pybind11;

namespace ProxygenBinding {

/**
 * Request data structure passed to Python callback
 */
struct RequestData {
  std::string method;
  std::string url;
  std::string path;
  std::string query_string;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
  std::string http_version;
};

/**
 * Response data structure returned from Python callback
 */
struct ResponseData {
  int status_code;
  std::string status_message;
  std::unordered_map<std::string, std::string> headers;
  std::string body;
};

/**
 * Python callback type for handling HTTP requests synchronously (DEPRECATED)
 * Takes RequestData and returns ResponseData immediately.
 *
 * WARNING: This blocks the Proxygen thread while Python processes the request.
 * Use PythonAsyncRequestCallback instead for better concurrency.
 */
using PythonRequestCallback = std::function<ResponseData(const RequestData&)>;

/**
 * Python async callback type for handling HTTP requests asynchronously
 * Takes RequestData and returns a Python coroutine/awaitable.
 *
 * The coroutine should resolve to ResponseData when complete.
 * This is the recommended callback type for Django/ASGI integration as it
 * doesn't block Proxygen threads while waiting for async operations.
 */
using PythonAsyncRequestCallback =
    std::function<py::object(const RequestData&)>;

/**
 * RequestHandler implementation that bridges Proxygen to Python
 *
 * Supports both synchronous and asynchronous Python callbacks:
 * - Synchronous: Blocks Proxygen thread until response is ready
 * - Asynchronous: Schedules coroutine on event loop, returns immediately
 */
class PythonRequestHandler : public proxygen::RequestHandler {
 public:
  // Constructor for synchronous callback (deprecated)
  explicit PythonRequestHandler(PythonRequestCallback callback);

  // Constructor for asynchronous callback (recommended)
  explicit PythonRequestHandler(
      PythonAsyncRequestCallback async_callback,
      bool is_async);

  void onRequest(
      std::unique_ptr<proxygen::HTTPMessage> headers) noexcept override;

  void onBody(std::unique_ptr<folly::IOBuf> body) noexcept override;

  void onEOM() noexcept override;

  void onUpgrade(proxygen::UpgradeProtocol proto) noexcept override;

  void requestComplete() noexcept override;

  void onError(proxygen::ProxygenError err) noexcept override;

 private:
  // Request handling
  void handleRequestSync();
  void handleRequestAsync();
  RequestData buildRequestData();
  void scheduleCoroutine(py::object coro);

  // Response sending
  void sendResponse(const ResponseData& response);

  // Callback storage - only one is set
  PythonRequestCallback sync_callback_;
  PythonAsyncRequestCallback async_callback_;
  bool is_async_;

  // Request data
  std::unique_ptr<proxygen::HTTPMessage> request_headers_;
  std::unique_ptr<folly::IOBuf> request_body_;

  // Proxygen event base for thread-safe response sending
  folly::EventBase* proxygen_event_base_{nullptr};
};

} // namespace ProxygenBinding

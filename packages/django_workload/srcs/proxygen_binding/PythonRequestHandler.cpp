/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "PythonRequestHandler.h"

#include <folly/io/IOBuf.h>
#include <glog/logging.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

// Debug logging macros - only enabled in debug builds
// Define PROXYGEN_BINDING_DEBUG to enable detailed logging
#ifdef PROXYGEN_BINDING_DEBUG
#define DEBUG_LOG(fmt, ...)                                            \
  do {                                                                 \
    std::fprintf(stderr, "[PROXYGEN_DEBUG] " fmt "\n", ##__VA_ARGS__); \
    std::fflush(stderr);                                               \
  } while (0)

#define DEBUG_TIMER_START() std::chrono::steady_clock::now()

#define DEBUG_TIMER_END(start, operation)                                  \
  do {                                                                     \
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>( \
                        std::chrono::steady_clock::now() - (start))        \
                        .count();                                          \
    std::fprintf(                                                          \
        stderr, "[PROXYGEN_DEBUG] %s took %ldms\n", operation, duration);  \
    std::fflush(stderr);                                                   \
  } while (0)
#else
#define DEBUG_LOG(fmt, ...) \
  do {                      \
  } while (0)
#define DEBUG_TIMER_START() std::chrono::steady_clock::time_point()
#define DEBUG_TIMER_END(start, operation) \
  do {                                    \
  } while (0)
#endif

namespace ProxygenBinding {

// Constructor for synchronous callback (deprecated)
PythonRequestHandler::PythonRequestHandler(PythonRequestCallback callback)
    : sync_callback_(std::move(callback)),
      async_callback_(nullptr),
      is_async_(false) {}

// Constructor for asynchronous callback (recommended)
PythonRequestHandler::PythonRequestHandler(
    PythonAsyncRequestCallback async_callback,
    bool is_async)
    : sync_callback_(nullptr),
      async_callback_(std::move(async_callback)),
      is_async_(is_async) {}

void PythonRequestHandler::onRequest(
    std::unique_ptr<proxygen::HTTPMessage> headers) noexcept {
  request_headers_ = std::move(headers);

  // Capture the Proxygen event base from this thread
  // This is the event base we'll use to marshal responses back
  proxygen_event_base_ = folly::EventBaseManager::get()->getEventBase();
  DEBUG_LOG(
      "[INIT] Captured event base from Proxygen thread: %p",
      static_cast<void*>(proxygen_event_base_));
}

void PythonRequestHandler::onBody(std::unique_ptr<folly::IOBuf> body) noexcept {
  if (request_body_) {
    request_body_->prependChain(std::move(body));
  } else {
    request_body_ = std::move(body);
  }
}

void PythonRequestHandler::onEOM() noexcept {
  try {
    if (is_async_) {
      handleRequestAsync();
    } else {
      handleRequestSync();
    }
  } catch (const std::exception& e) {
    LOG(ERROR) << "Exception in PythonRequestHandler: " << e.what();
    proxygen::ResponseBuilder(downstream_)
        .status(500, "Internal Server Error")
        .body(folly::IOBuf::copyBuffer("Internal Server Error\n"))
        .sendWithEOM();
  }
}

void PythonRequestHandler::onUpgrade(
    proxygen::UpgradeProtocol /*proto*/) noexcept {}

void PythonRequestHandler::requestComplete() noexcept {
  delete this;
}

void PythonRequestHandler::onError(proxygen::ProxygenError err) noexcept {
  LOG(ERROR) << "Proxygen error: " << proxygen::getErrorString(err);
  delete this;
}

void PythonRequestHandler::handleRequestSync() {
  RequestData req = buildRequestData();

  ResponseData response;
  {
    py::gil_scoped_acquire acquire;
    try {
      response = sync_callback_(req);
    } catch (const py::error_already_set& e) {
      LOG(ERROR) << "Python exception in request handler: " << e.what();
      response.status_code = 500;
      response.status_message = "Internal Server Error";
      response.body = "Internal Server Error\n";
    }
  }

  sendResponse(response);
}

void PythonRequestHandler::handleRequestAsync() {
  RequestData req = buildRequestData();

  // CRITICAL: Keep py::object coro alive until scheduleCoroutine completes
  // to avoid GIL issues during destruction
  {
    py::gil_scoped_acquire acquire;
    try {
      // Call async_callback which returns a coroutine
      py::object coro = async_callback_(req);

      // Schedule the coroutine while still holding GIL
      // scheduleCoroutine will also acquire GIL (nested is OK)
      scheduleCoroutine(coro);

      // coro will be destroyed here while GIL is still held
    } catch (const py::error_already_set& e) {
      LOG(ERROR) << "Python exception in async callback: " << e.what();
      ResponseData error_response;
      error_response.status_code = 500;
      error_response.status_message = "Internal Server Error";
      error_response.body = "Internal Server Error\n";
      sendResponse(error_response);
      return;
    }
  }
  // GIL is released here, after coro has been safely destroyed
}

void PythonRequestHandler::scheduleCoroutine(py::object coro) {
  // Acquire GIL to interact with Python
  py::gil_scoped_acquire acquire;

  try {
    // Get the event loop manager module
    py::object event_loop_manager = py::module::import("event_loop_manager");
    py::object get_event_loop_manager =
        event_loop_manager.attr("get_event_loop_manager");

    // Get or create the background event loop for this thread
    py::object manager = get_event_loop_manager();
    py::object background_loop = manager.attr("get_or_create_loop")();

    // Create callback that will be called when coroutine completes
    // This callback will run in the background event loop thread
    auto* self = this;

    auto done_callback = py::cpp_function([self](py::object future) {
      // This callback runs in the background Python thread
      // Extract response while holding GIL, then release before sending

      DEBUG_LOG("[CALLBACK] Done callback starting...");
      auto callback_start = DEBUG_TIMER_START();

      ResponseData response;

      {
        // Acquire GIL only for extracting Python objects
        py::gil_scoped_acquire acquire_inner;
        DEBUG_LOG("[CALLBACK] GIL acquired");

        try {
          // Get the result (ResponseData) from the future
          py::object result = future.attr("result")();
          response = result.cast<ResponseData>();
          DEBUG_LOG(
              "[CALLBACK] Response extracted (status=%d)",
              response.status_code);

        } catch (const py::error_already_set& e) {
          DEBUG_LOG("[CALLBACK] Exception in coroutine: %s", e.what());
          response.status_code = 500;
          response.status_message = "Internal Server Error";
          response.body = "Internal Server Error\n";
        } catch (const std::exception& e) {
          DEBUG_LOG("[CALLBACK] C++ exception: %s", e.what());
          response.status_code = 500;
          response.status_message = "Internal Server Error";
          response.body = "Internal Server Error\n";
        }

        // GIL is released at end of this scope
        DEBUG_LOG("[CALLBACK] Releasing GIL...");
      }

      // GIL is now released - marshal response back to Proxygen thread
      DEBUG_LOG("[CALLBACK] Marshaling response back to Proxygen thread...");
      auto marshal_start = DEBUG_TIMER_START();

      // Get the Proxygen event base
      auto* event_base = self->proxygen_event_base_;

      if (event_base) {
        DEBUG_LOG(
            "[CALLBACK] Event base available (%p), scheduling on Proxygen thread...",
            static_cast<void*>(event_base));

        // Schedule sendResponse to run on the Proxygen thread
        // This is THE KEY FIX for the 1-second delay!
        event_base->runInEventBaseThread([self, response]() {
          DEBUG_LOG(
              "[CALLBACK] Now running in Proxygen thread, sending response...");
          self->sendResponse(response);
          DEBUG_LOG("[CALLBACK] Response sent from Proxygen thread!");
        });

        DEBUG_TIMER_END(marshal_start, "[CALLBACK] Response marshaling");
        DEBUG_TIMER_END(callback_start, "[CALLBACK] Total callback");
      } else {
        // Fallback: send directly (will have 1s delay but won't crash)
        LOG(WARNING)
            << "[CALLBACK] No event base available, sending directly (will be slow)";
        self->sendResponse(response);
      }
    });

    // Schedule the coroutine on the background event loop (NON-BLOCKING!)
    // This is the key change - we no longer block here
    DEBUG_LOG("[SCHEDULE] Scheduling coroutine on background loop...");

    background_loop.attr("schedule_coroutine")(coro, done_callback);

    // We return immediately - the Proxygen thread is NOT blocked!
    // The coroutine will run concurrently in the background event loop
    // and the response will be sent when it completes.
    DEBUG_LOG("[SCHEDULE] Coroutine scheduled, returning to Proxygen thread");

  } catch (const py::error_already_set& e) {
    LOG(ERROR) << "Failed to schedule coroutine: " << e.what();
    ResponseData error_response;
    error_response.status_code = 500;
    error_response.status_message = "Internal Server Error";
    error_response.body = "Internal Server Error\n";
    sendResponse(error_response);
  }
}

void PythonRequestHandler::sendResponse(const ResponseData& response) {
  DEBUG_LOG("[SEND] sendResponse starting (status=%d)", response.status_code);
  auto start_time = DEBUG_TIMER_START();

  proxygen::ResponseBuilder builder(downstream_);
  builder.status(response.status_code, response.status_message);

  for (const auto& [key, value] : response.headers) {
    builder.header(key, value);
  }

  if (!response.body.empty()) {
    builder.body(folly::IOBuf::copyBuffer(response.body));
  }

  DEBUG_LOG("[SEND] Calling sendWithEOM()...");
  auto send_eom_start = DEBUG_TIMER_START();

  builder.sendWithEOM();

  DEBUG_TIMER_END(send_eom_start, "[SEND] sendWithEOM");
  DEBUG_TIMER_END(start_time, "[SEND] Total sendResponse");
}

RequestData PythonRequestHandler::buildRequestData() {
  RequestData data;

  if (request_headers_) {
    data.method = request_headers_->getMethodString();
    data.url = request_headers_->getURL();
    data.path = request_headers_->getPath();
    data.query_string = request_headers_->getQueryString();

    auto httpVersion = request_headers_->getVersionString();
    data.http_version = std::string(httpVersion.data(), httpVersion.size());

    request_headers_->getHeaders().forEach(
        [&data](const std::string& header, const std::string& value) {
          data.headers[header] = value;
        });
  }

  if (request_body_) {
    auto bodyRange = request_body_->coalesce();
    data.body = std::string(
        reinterpret_cast<const char*>(bodyRange.data()), bodyRange.size());
  }

  return data;
}

} // namespace ProxygenBinding

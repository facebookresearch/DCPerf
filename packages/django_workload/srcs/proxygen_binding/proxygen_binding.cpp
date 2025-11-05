/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <pybind11/functional.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <folly/SocketAddress.h>
#include <folly/portability/Unistd.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <proxygen/httpserver/HTTPServer.h>
#include <proxygen/httpserver/RequestHandlerFactory.h>

#include <memory>
#include <thread>

#include "PythonRequestHandler.h"
#include "PythonRequestHandlerFactory.h"

namespace py = pybind11;
using namespace ProxygenBinding;
using namespace proxygen;

/**
 * Wrapper class for Proxygen HTTP Server
 *
 * Supports both synchronous and asynchronous callbacks.
 */
class ProxygenServer {
 public:
  // Constructor for synchronous callback (deprecated)
  ProxygenServer(
      const std::string& ip,
      int port,
      int threads,
      PythonRequestCallback callback)
      : ip_(ip),
        port_(port),
        threads_(threads),
        sync_callback_(std::move(callback)),
        async_callback_(nullptr),
        is_async_(false),
        server_(nullptr),
        server_thread_(nullptr) {}

  // Constructor for asynchronous callback (recommended)
  ProxygenServer(
      const std::string& ip,
      int port,
      int threads,
      PythonAsyncRequestCallback async_callback,
      bool is_async)
      : ip_(ip),
        port_(port),
        threads_(threads),
        sync_callback_(nullptr),
        async_callback_(std::move(async_callback)),
        is_async_(is_async),
        server_(nullptr),
        server_thread_(nullptr) {}

  ~ProxygenServer() {
    stop();
  }

  void start() {
    if (server_ != nullptr) {
      throw std::runtime_error("Server is already running");
    }

    py::gil_scoped_release release;

    // Configure socket options to enable SO_REUSEPORT
    // This allows multiple worker processes to bind to the same port
    // Essential for uWSGI multi-worker setup where each worker runs its own
    // Proxygen server
    folly::SocketOptionMap socket_options;
    socket_options[{SOL_SOCKET, SO_REUSEPORT}] = 1;

    HTTPServer::IPConfig ip_config(
        folly::SocketAddress(ip_, port_, true), HTTPServer::Protocol::HTTP);
    ip_config.acceptorSocketOptions = socket_options;

    std::vector<HTTPServer::IPConfig> IPs = {ip_config};

    int actual_threads = threads_;
    if (actual_threads <= 0) {
      actual_threads = sysconf(_SC_NPROCESSORS_ONLN);
      if (actual_threads <= 0) {
        actual_threads = 1;
      }
    }

    HTTPServerOptions options;
    options.threads = static_cast<size_t>(actual_threads);
    options.idleTimeout = std::chrono::milliseconds(60000);
    options.enableContentCompression = false;

    // Create handler factory based on mode (sync or async)
    if (is_async_) {
      options.handlerFactories =
          RequestHandlerChain()
              .addThen(
                  std::make_unique<PythonRequestHandlerFactory>(
                      async_callback_, is_async_))
              .build();
      LOG(INFO) << "Using async callback mode";
    } else {
      options.handlerFactories =
          RequestHandlerChain()
              .addThen(
                  std::make_unique<PythonRequestHandlerFactory>(sync_callback_))
              .build();
      LOG(INFO) << "Using sync callback mode";
    }

    options.initialReceiveWindow = uint32_t(1 << 20);
    options.receiveStreamWindowSize = uint32_t(1 << 20);
    options.receiveSessionWindowSize = 10 * (1 << 20);
    options.listenBacklog = 1024;

    server_ = std::make_unique<HTTPServer>(std::move(options));
    server_->bind(IPs);

    server_thread_ =
        std::make_unique<std::thread>([this]() { server_->start(); });

    LOG(INFO) << "Proxygen server started on " << ip_ << ":" << port_
              << " with " << actual_threads
              << " threads (SO_REUSEPORT enabled)";
  }

  void stop() {
    if (server_) {
      py::gil_scoped_release release;

      server_->stop();

      if (server_thread_ && server_thread_->joinable()) {
        server_thread_->join();
      }

      server_.reset();
      server_thread_.reset();

      LOG(INFO) << "Proxygen server stopped";
    }
  }

  void wait() {
    // Periodically check for Python signals (like KeyboardInterrupt)
    // instead of blocking indefinitely in join()
    while (server_thread_ && server_thread_->joinable() && server_) {
      {
        py::gil_scoped_release release;
        // Sleep briefly to allow signal delivery
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      // Check if Python signal occurred (e.g., Ctrl+C)
      if (PyErr_CheckSignals() != 0) {
        // Stop the server and let the exception propagate
        stop();
        throw py::error_already_set();
      }
    }

    // Final join to ensure thread cleanup
    if (server_thread_ && server_thread_->joinable()) {
      py::gil_scoped_release release;
      server_thread_->join();
    }
  }

 private:
  std::string ip_;
  int port_;
  int threads_;
  PythonRequestCallback sync_callback_;
  PythonAsyncRequestCallback async_callback_;
  bool is_async_;
  std::unique_ptr<HTTPServer> server_;
  std::unique_ptr<std::thread> server_thread_;
};

PYBIND11_MODULE(proxygen_binding, m) {
  m.doc() = "Python bindings for Proxygen HTTP server";

  py::class_<RequestData>(m, "RequestData")
      .def(py::init<>())
      .def_readwrite("method", &RequestData::method)
      .def_readwrite("url", &RequestData::url)
      .def_readwrite("path", &RequestData::path)
      .def_readwrite("query_string", &RequestData::query_string)
      .def_readwrite("headers", &RequestData::headers)
      .def_readwrite("body", &RequestData::body)
      .def_readwrite("http_version", &RequestData::http_version)
      .def("__repr__", [](const RequestData& req) {
        return "<RequestData method=" + req.method + " path=" + req.path + ">";
      });

  py::class_<ResponseData>(m, "ResponseData")
      .def(py::init<>())
      .def_readwrite("status_code", &ResponseData::status_code)
      .def_readwrite("status_message", &ResponseData::status_message)
      .def_readwrite("headers", &ResponseData::headers)
      .def_readwrite("body", &ResponseData::body)
      .def("__repr__", [](const ResponseData& resp) {
        return "<ResponseData status_code=" + std::to_string(resp.status_code) +
            ">";
      });

  py::class_<ProxygenServer>(m, "ProxygenServer")
      // Synchronous callback constructor (deprecated)
      .def(
          py::init<const std::string&, int, int, PythonRequestCallback>(),
          py::arg("ip") = "127.0.0.1",
          py::arg("port") = 8000,
          py::arg("threads") = 0,
          py::arg("callback"),
          "Create server with synchronous callback (deprecated - use async_callback)")
      // Asynchronous callback constructor (recommended)
      .def(
          py::init<
              const std::string&,
              int,
              int,
              PythonAsyncRequestCallback,
              bool>(),
          py::arg("ip") = "127.0.0.1",
          py::arg("port") = 8000,
          py::arg("threads") = 0,
          py::arg("async_callback"),
          py::arg("is_async") = true,
          "Create server with asynchronous callback (recommended)")
      .def("start", &ProxygenServer::start, "Start the HTTP server")
      .def("stop", &ProxygenServer::stop, "Stop the HTTP server")
      .def(
          "wait",
          &ProxygenServer::wait,
          "Wait for the server thread to finish");

  m.def(
      "init_logging",
      []() {
        google::InitGoogleLogging("proxygen_binding");
        LOG(INFO) << "Proxygen binding logging initialized";
      },
      "Initialize Google logging for Proxygen");
}

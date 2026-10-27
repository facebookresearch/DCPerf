// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <chrono>
#include <cstdlib>
#include <iostream>

#include <gflags/gflags.h>
#include <folly/init/Init.h>
#include <folly/io/async/ScopedEventBaseThread.h>

#include "MockServicesClient.h"

DEFINE_string(host, "localhost", "Mock-service hostname.");
DEFINE_int32(port, 21222, "Mock-service port.");
DEFINE_int32(timeout_ms, 100, "Timeout for each connectivity probe.");
DEFINE_int32(retries, 0, "Retries after the initial connectivity attempt.");
DEFINE_int32(initial_backoff_ms, 100, "Initial retry interval.");
DEFINE_int32(max_backoff_ms, 1000, "Maximum retry interval.");

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  if (FLAGS_port <= 0 || FLAGS_port > 65535) {
    std::cerr << "Invalid mock-service port" << std::endl;
    return EXIT_FAILURE;
  }
  try {
    folly::ScopedEventBaseThread evb;
    ranking::MockServicesClient client(
        evb.getEventBase(),
        FLAGS_host,
        static_cast<uint16_t>(FLAGS_port),
        std::chrono::milliseconds(0),
        ranking::MockServicesConnectionOptions{
            FLAGS_retries,
            std::chrono::milliseconds(FLAGS_initial_backoff_ms),
            std::chrono::milliseconds(FLAGS_max_backoff_ms),
            std::chrono::milliseconds(FLAGS_timeout_ms)});
    std::cout << "mock_services getStatus succeeded" << std::endl;
    return EXIT_SUCCESS;
  } catch (const std::exception& e) {
    std::cerr << "mock_services readiness probe failed: " << e.what()
              << std::endl;
    return EXIT_FAILURE;
  }
}

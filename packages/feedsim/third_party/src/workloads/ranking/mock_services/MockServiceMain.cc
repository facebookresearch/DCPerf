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
#include <memory>
#include <thread>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <folly/init/Init.h>

#include "thrift/lib/cpp2/server/ThriftServer.h"

#include "LatencyHistogram.h"
#include "MockServiceHandler.h"

#include "SilesiaLoader.h"

namespace mock_services {
extern feedsim::LatencyHistogram g_handler_requested_us;
extern feedsim::LatencyHistogram g_handler_actual_us;
extern feedsim::LatencyHistogram g_handler_effective_us;
} // namespace mock_services

DEFINE_int32(port, 21222, "Port for the mock_services Thrift server.");
DEFINE_int32(
    mock_io_threads,
    0,
    "Number of IO worker threads. 0 = std::thread::hardware_concurrency().");
DEFINE_string(
    silesia_dir,
    "",
    "Required. Path to the Silesia corpus directory used for response bytes.");

// Latency-shaping flags are DEFINE'd in MockServiceHandler.cc (where they're
// consumed) so the mock_service_handler cpp_library links cleanly on its
// own; declared here so the main-side LOG line can read them.
DECLARE_int32(latency_cap_us);
DECLARE_int32(latency_offset_us);
DECLARE_int32(latency_skip_threshold_us);

// NOTE: TLS support intentionally not wired here. The open-source FBThrift
// v2026.01.05.00 Rocket transport requires a fizz-based AsyncTransport (not
// plain AsyncSSLSocket) on the client side; without it, TLS-REQUIRED servers
// reject the first Rocket setup frame before SSL handshake even begins. See
// the Encryption gap analysis in the t31 memo for the closure plan.

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);

  if (FLAGS_silesia_dir.empty()) {
    LOG(ERROR) << "--silesia_dir is required";
    return EXIT_FAILURE;
  }

  auto silesia = std::make_shared<ranking::SilesiaLoader>();
  if (!silesia->loadDirectory(FLAGS_silesia_dir)) {
    LOG(ERROR) << "Failed to load Silesia corpus from: " << FLAGS_silesia_dir;
    return EXIT_FAILURE;
  }

  int io_threads = FLAGS_mock_io_threads > 0
      ? FLAGS_mock_io_threads
      : static_cast<int>(std::thread::hardware_concurrency());

  auto handler = std::make_shared<mock_services::MockServiceHandler>(silesia);
  auto server = std::make_shared<apache::thrift::ThriftServer>();
  server->setInterface(handler);
  server->setPort(FLAGS_port);
  server->setNumIOWorkerThreads(io_threads);

  LOG(INFO) << "mock_services listening on port " << FLAGS_port
            << " with " << io_threads << " IO worker threads"
            << "; Silesia corpus from " << FLAGS_silesia_dir
            << " (" << silesia->numFiles() << " files, "
            << (silesia->totalSize() / (1024 * 1024)) << " MB)";

  LOG(INFO) << "latency shaping: cap_us=" << FLAGS_latency_cap_us
            << " offset_us=" << FLAGS_latency_offset_us
            << " skip_threshold_us=" << FLAGS_latency_skip_threshold_us;

  // Background dump of the per-request requested-vs-effective-vs-actual
  // latency histograms so we can compare what rpc_dist.json asks for,
  // what the cap/offset shaping decides to actually wait for, and what
  // the handler ends up spending wall-time on.
  std::thread debug_dump_thread([]() {
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      mock_services::g_handler_requested_us.dump("mock_handler_requested");
      mock_services::g_handler_effective_us.dump("mock_handler_effective");
      mock_services::g_handler_actual_us.dump("mock_handler_actual");
    }
  });
  debug_dump_thread.detach();

  server->serve();
  return 0;
}

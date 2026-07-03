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

#include <cstdlib>
#include <memory>
#include <thread>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <folly/init/Init.h>

#include "thrift/lib/cpp2/server/ThriftServer.h"

#include "MockServiceHandler.h"

#include "SilesiaLoader.h"

DEFINE_int32(port, 21222, "Port for the mock_services Thrift server.");
DEFINE_int32(
    mock_io_threads,
    0,
    "Number of IO worker threads. 0 = std::thread::hardware_concurrency().");
DEFINE_string(
    silesia_dir,
    "",
    "Required. Path to the Silesia corpus directory used for response bytes.");

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

  server->serve();
  return 0;
}

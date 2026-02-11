/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/Format.h>
#include <folly/init/Init.h>
#include <folly/portability/GFlags.h>

#include "UcacheBenchClient.h"

DECLARE_string(server_host);
DECLARE_uint32(server_port);
DECLARE_uint32(duration_seconds);
DECLARE_uint32(warmup_seconds);
DECLARE_bool(verbose);

using namespace facebook::ucachebench;

int main(int argc, char** argv) {
  folly::init(&argc, &argv, true);

  if (FLAGS_verbose) {
    printf(
        "Starting UcacheBench client targeting %s:%u\n",
        FLAGS_server_host.c_str(),
        FLAGS_server_port);
    printf(
        "Warmup: %us, Test: %us\n",
        FLAGS_warmup_seconds,
        FLAGS_duration_seconds);
    fflush(stdout);
  }

  try {
    UcacheBenchClient client;

    // Warmup phase
    auto warmupResults = client.warmup();

    // Benchmark phase
    auto results = client.runBenchmark();

    // Include warmup results in the final results
    results.warmupResults = warmupResults;

    // Print results (now includes warmup)
    client.printResults(results);

    return 0;
  } catch (const std::exception& ex) {
    printf("Client error: %s\n", ex.what());
    return 1;
  }
}

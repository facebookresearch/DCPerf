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

static bool ValidateServerPort(const char* flagname, uint32_t value) {
  if (value > 0 && value < 65536) {
    return true;
  }
  printf(
      "Invalid value for --%s: %u (must be between 1 and 65535)\n",
      flagname,
      value);
  return false;
}
DEFINE_validator(server_port, &ValidateServerPort);

// Admin server flag for multi-client coordination
// Note: admin_host is not needed - we use server_host since admin server
// runs on the same machine as the cache server
DEFINE_uint32(
    admin_port,
    0,
    "Admin server port for multi-client coordination (0 = disabled)");

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

    // Connect to admin server if configured (uses server_host since admin runs
    // on same machine)
    if (FLAGS_admin_port > 0) {
      printf(
          "Connecting to admin server at %s:%u for multi-client coordination\n",
          FLAGS_server_host.c_str(),
          FLAGS_admin_port);
      if (!client.connectToAdmin(
              FLAGS_server_host, static_cast<uint16_t>(FLAGS_admin_port))) {
        printf("Failed to connect to admin server\n");
        return 1;
      }
    }

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

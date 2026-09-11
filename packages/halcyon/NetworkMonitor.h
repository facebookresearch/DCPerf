// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <cstdint>
#include <string>

#include <folly/Subprocess.h>

#pragma once

namespace facebook::halcyon {

struct NetworkStatCounters {
  uint64_t ts = 0;
  uint64_t recvBytes = 0;
  uint64_t recvPackets = 0;
  uint64_t sendBytes = 0;
  uint64_t sendPackets = 0;
};

class NetworkMonitor {
 public:
  std::string interface;
  int linkSpeed;
  explicit NetworkMonitor(const std::string& device = "eth0");
  double getThroughput();
  double getThroughputFromBeginning();
  int getLinkSpeed();
  void clearCounters();

 private:
  NetworkStatCounters initialCounts;
  NetworkStatCounters lastCounts;
  NetworkStatCounters getCounts();
};

} // namespace facebook::halcyon

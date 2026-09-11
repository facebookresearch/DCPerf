// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <cstdint>

#pragma once

namespace facebook::halcyon {

struct CpuStatCounters {
  uint64_t ts = 0;
  uint64_t user = 0;
  uint64_t nice = 0;
  uint64_t system = 0;
  uint64_t idle = 0;
  uint64_t iowait = 0;
  int nCpu = 0;
};

class CpuMonitor {
 public:
  CpuMonitor();
  int ticksPerSec;
  double getUtilization();
  double getUtilizationFromBeginning();
  void clearCounters();
  CpuStatCounters getCounts();

 private:
  CpuStatCounters initialCounts;
  CpuStatCounters lastCounts;
};

} // namespace facebook::halcyon

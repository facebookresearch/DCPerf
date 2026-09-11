// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "CpuMonitor.h"
#include "Common.h"

#include <fstream>
#include <string>
#include <vector>

#include <folly/Format.h>
#include <folly/String.h>

namespace facebook::halcyon {

using namespace folly;
using namespace std;

CpuMonitor::CpuMonitor() {
  ticksPerSec = sysconf(_SC_CLK_TCK);
  initialCounts = CpuMonitor::getCounts();
  lastCounts = initialCounts;
}

CpuStatCounters CpuMonitor::getCounts() {
  ifstream statsfile("/proc/stat");
  int cpus = 0;
  bool foundCpu = false;
  CpuStatCounters counts;
  counts.ts = current_nano();
  for (string line; getline(statsfile, line);) {
    if (line.rfind("cpu") == 0) { // starts with cpu
      if (foundCpu) {
        cpus++;
      } else {
        foundCpu = true;
        vector<StringPiece> pieces;
        split(' ', line, pieces);
        counts.user = stoul(pieces.at(2).toString());
        counts.nice = stoul(pieces.at(3).toString());
        counts.system = stoul(pieces.at(4).toString());
        counts.idle = stoul(pieces.at(5).toString());
        counts.iowait = stoul(pieces.at(6).toString());
      }
    }
  }
  counts.nCpu = cpus;
  return counts;
}

double CpuMonitor::getUtilization() {
  CpuStatCounters now = CpuMonitor::getCounts();
  double elapsed = (now.ts - lastCounts.ts) / 1e9;
  uint64_t idleDiff = now.idle - lastCounts.idle;
  if (elapsed <= 0 || idleDiff == 0) {
    return 0;
  }
  double idlePerc = idleDiff / elapsed / ticksPerSec / now.nCpu;
  double utilization = 100 * (1 - idlePerc);
  lastCounts = now;
  return utilization;
}

double CpuMonitor::getUtilizationFromBeginning() {
  CpuStatCounters now = CpuMonitor::getCounts();
  double elapsed = (now.ts - initialCounts.ts) / 1e9;
  uint64_t idleDiff = now.idle - initialCounts.idle;
  if (elapsed <= 0 || idleDiff == 0) {
    return 0;
  }
  double idlePerc = idleDiff / elapsed / ticksPerSec / now.nCpu;
  double utilization = 100 * (1 - idlePerc);
  return utilization;
}

void CpuMonitor::clearCounters() {
  initialCounts = CpuMonitor::getCounts();
  lastCounts = initialCounts;
}

} // namespace facebook::halcyon

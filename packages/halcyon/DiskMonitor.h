// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <cstdint>

#pragma once

namespace facebook::halcyon {
// for Linux purposes, as in /proc/diskstats
const int kBytesPerSector = 512;

struct DiskStatCounters {
  uint64_t ts = 0;
  uint64_t readIos = 0;
  uint64_t readBytes = 0;
  uint64_t readMsec = 0;
  uint64_t writeIos = 0;
  uint64_t writeBytes = 0;
  uint64_t writeMsec = 0;
  int inflightIos = 0;
  uint64_t ioMsec = 0;
  int nDisks = 0;
};

class DiskMonitor {
 public:
  DiskMonitor();
  double getUtilization();
  double getUtilizationFromBeginning();
  void clearCounters();

 private:
  DiskStatCounters initialCounts;
  DiskStatCounters lastCounts;
  DiskStatCounters getCounts();
};

} // namespace facebook::halcyon

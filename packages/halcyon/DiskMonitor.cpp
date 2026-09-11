// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "DiskMonitor.h"
#include "Common.h"

#include <fstream>
#include <string>
#include <vector>

#include <folly/Format.h>
#include <folly/String.h>
#include <re2/re2.h>

namespace facebook::halcyon {

using namespace folly;
using namespace std;

DiskMonitor::DiskMonitor() {
  initialCounts = DiskMonitor::getCounts();
  lastCounts = initialCounts;
}

DiskStatCounters DiskMonitor::getCounts() {
  ifstream statsfile("/proc/diskstats");
  DiskStatCounters counts;
  counts.ts = current_nano();
  int nDisks = 0;
  // Only match base devices, no partitions
  re2::RE2 devRegex("(sd[b-z]|sda[a-k]) ");
  re2::RE2 multiSpace("\\s+");
  for (string line; getline(statsfile, line);) {
    if (re2::RE2::PartialMatch(line, devRegex)) {
      // Remove multichar whitespace
      re2::RE2::GlobalReplace(&line, multiSpace, " ");
      vector<StringPiece> pieces;
      split(' ', line, pieces);
      counts.readIos += stoul(pieces.at(4).toString());
      counts.readBytes += stoul(pieces.at(6).toString()) * kBytesPerSector;
      counts.readMsec += stoul(pieces.at(7).toString());
      counts.writeIos += stoul(pieces.at(8).toString());
      counts.writeBytes += stoul(pieces.at(10).toString()) * kBytesPerSector;
      counts.writeMsec += stoul(pieces.at(11).toString());
      counts.inflightIos += stoi(pieces.at(12).toString());
      uint64_t ioMsec = stoul(pieces.at(13).toString());
      counts.ioMsec += ioMsec;
      nDisks++;
    }
  }
  counts.nDisks = nDisks;
  return counts;
}

double DiskMonitor::getUtilization() {
  DiskStatCounters now = DiskMonitor::getCounts();
  double elapsed = (now.ts - lastCounts.ts) / 1e9;
  double ioSecs = (now.ioMsec - lastCounts.ioMsec) / 1000.0;
  if (elapsed <= 0 || ioSecs == 0) {
    return 0;
  }
  double utilization = 100 * ioSecs / elapsed / now.nDisks;
  lastCounts = now;
  return utilization;
}

double DiskMonitor::getUtilizationFromBeginning() {
  DiskStatCounters now = DiskMonitor::getCounts();
  double elapsed = (now.ts - initialCounts.ts) / 1e9;
  double ioSecs = (now.ioMsec - initialCounts.ioMsec) / 1000.0;
  if (elapsed <= 0 || ioSecs == 0) {
    return 0;
  }
  double utilization = 100 * ioSecs / elapsed / now.nDisks;
  return utilization;
}

void DiskMonitor::clearCounters() {
  initialCounts = DiskMonitor::getCounts();
  lastCounts = initialCounts;
}

} // namespace facebook::halcyon

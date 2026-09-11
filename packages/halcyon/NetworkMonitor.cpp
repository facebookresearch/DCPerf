// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "NetworkMonitor.h"

#include "Common.h"

#include <fstream>
#include <string>
#include <vector>

#include <folly/Format.h>
#include <folly/String.h>
#include <re2/re2.h>

namespace facebook::halcyon {

NetworkMonitor::NetworkMonitor(const string& device) {
  interface = device;
  linkSpeed = 0;
  NetworkMonitor::clearCounters();
  // Determine link speed
  ifstream netSpeedFile(sformat("/sys/class/net/{}/speed", interface));
  string contents(
      (istreambuf_iterator<char>(netSpeedFile)), (istreambuf_iterator<char>()));
  linkSpeed = stoi(contents);
  if (linkSpeed == -1) {
    LOG(WARNING) << "Could not get link speed, assuming 25000 Mb/s";
    linkSpeed = 25'000;
  } else {
    LOG(INFO) << sformat("Link speed is {} Mb/s", linkSpeed);
  }
}

int NetworkMonitor::getLinkSpeed() {
  return linkSpeed;
}

NetworkStatCounters NetworkMonitor::getCounts() {
  ifstream netdevfile("/proc/net/dev");
  NetworkStatCounters counts;
  counts.ts = current_nano();
  for (string line; getline(netdevfile, line);) {
    if (line.find(interface) == string::npos) {
      continue;
    }
    vector<StringPiece> pieces;
    split(' ', line, pieces, true);
    counts.recvBytes = stoul(pieces.at(1).toString());
    counts.recvPackets = stoul(pieces.at(2).toString());
    counts.sendBytes = stoul(pieces.at(9).toString());
    counts.sendPackets = stoul(pieces.at(10).toString());
  }
  return counts;
}

double NetworkMonitor::getThroughput() {
  NetworkStatCounters now = NetworkMonitor::getCounts();
  double elapsed = (now.ts - lastCounts.ts) / 1e9;
  if (elapsed <= 0) {
    return 0;
  }
  double xput = ((now.recvBytes + now.sendBytes) -
                 (lastCounts.recvBytes + lastCounts.sendBytes)) /
      elapsed;
  lastCounts = now;
  return xput / 1e6;
}

double NetworkMonitor::getThroughputFromBeginning() {
  NetworkStatCounters now = NetworkMonitor::getCounts();
  double elapsed = (now.ts - initialCounts.ts) / 1e9;
  if (elapsed <= 0) {
    return 0;
  }
  double xput = ((now.recvBytes + now.sendBytes) -
                 (initialCounts.recvBytes + initialCounts.sendBytes)) /
      elapsed;
  return xput / 1e6;
}

void NetworkMonitor::clearCounters() {
  initialCounts = NetworkMonitor::getCounts();
  lastCounts = initialCounts;
}

} // namespace facebook::halcyon

// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <vector>
#include "Common.h"

namespace facebook::halcyon {

using namespace std;

/// A convenience class that holds the performance statistics
/// for all threads doing file I/O.  An instance of this gets
/// passed to all file I/O workers which access their unique
/// portion to update statistics.
class PerfStats {
 public:
  int nOperators; // typically the number of mount points
  int nWorkers; // typically the number of threads per disk
  vector<vector<IoStats>> istats;
  vector<vector<ChecksumStats>> cstats;
  vector<vector<MetadataStats>> mstats;
  vector<vector<bool>> activeIo;
  PerfStats(int operators, int workers);
};

class NetStats {
 public:
  int nOperators;
  vector<NetworkStats> nstats;
  vector<bool> activeIo;
  explicit NetStats(int operators);
};

} // namespace facebook::halcyon

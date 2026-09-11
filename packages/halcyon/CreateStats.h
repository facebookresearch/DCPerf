// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <vector>
#include "Common.h"

namespace facebook::halcyon {

using namespace std;

/// A convenience class that holds the file create statistics
/// for all threads doing fileset creation.  An instance of this
/// gets passed to all file I/O workers which access their unique
/// portion to update statistics.
class CreateStats {
 public:
  int nOperators; // typically the number of mount points
  int nWorkers; // typically the number of threads per disk
  bool stopNow; // set to true when we get an interrupt
  bool inProgress; // is the create process still running
  vector<vector<FilesetStats>> stats;
  CreateStats(int operators, int workers);
};

} // namespace facebook::halcyon

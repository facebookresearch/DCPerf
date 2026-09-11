// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "Common.h"

#include <string>
#include <vector>

namespace facebook::halcyon {

// Class for storing benchmark configuration information parsed from
// a JSON file.
class BenchmarkConfiguration {
 public:
  double readPerc;
  vector<IoSizeRange> readSizes;
  vector<IoSizeRange> writeSizes;
  vector<string> mountPoints;
  explicit BenchmarkConfiguration(string fname);
  /// Returns the total number of mount points listed in the config file
  int numMountPoints();
};

} // namespace facebook::halcyon

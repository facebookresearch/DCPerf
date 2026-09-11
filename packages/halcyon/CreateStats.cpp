// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "CreateStats.h"

namespace facebook::halcyon {

CreateStats::CreateStats(int operators, int workers) {
  nOperators = operators;
  nWorkers = workers;
  stopNow = false;
  inProgress = true;
  stats.resize(nOperators);
  for (int i = 0; i < nOperators; i++) {
    stats[i].resize(nWorkers);
  }
}

} // namespace facebook::halcyon

// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "PerfStats.h"

namespace facebook::halcyon {

PerfStats::PerfStats(int operators, int workers) {
  nOperators = operators;
  nWorkers = workers;
  istats.resize(nOperators);
  cstats.resize(nOperators);
  mstats.resize(nOperators);
  activeIo.resize(nOperators);
  for (int i = 0; i < nOperators; i++) {
    istats[i].resize(nWorkers);
    cstats[i].resize(nWorkers);
    mstats[i].resize(nWorkers);
    activeIo[i].resize(nWorkers);
    for (int j = 0; j < nWorkers; j++) {
      activeIo[i][j] = false;
    }
  }
}

NetStats::NetStats(int operators) {
  nOperators = operators;
  nstats.resize(nOperators);
  activeIo.resize(nOperators);
  for (int i = 0; i < nOperators; i++) {
    activeIo[i] = false;
  }
}

} // namespace facebook::halcyon

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <cea/chips/adsim/cpp2/server/dwarfs/GpuSpinWaitImpl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

namespace facebook::cea::chips::adsim::gpu_spin_wait {
namespace {

constexpr double kWaitUs = 500;
constexpr int kRepeats = 5;

struct Waits {
  int64_t min_iters = std::numeric_limits<int64_t>::max();
  double min_us = std::numeric_limits<double>::infinity();
};

// Waits kWaitUs kRepeats times on one signal, timed with steady_clock.
Waits timedWaits(Signal& signal, bool use_mwaitx) {
  const uint64_t ticks = busyTicks(kWaitUs);
  Waits w;
  for (int i = 0; kRepeats > i; ++i) {
    const auto t0 = std::chrono::steady_clock::now();
    const int64_t iters = waitForDispatch(signal, ticks, use_mwaitx);
    const double us = std::chrono::duration<double, std::micro>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    w.min_iters = std::min(w.min_iters, iters);
    w.min_us = std::min(w.min_us, us);
  }
  return w;
}

TEST(GpuSpinWaitTest, ZeroWaitCompletesOnSecondCheck) {
  // The first check finds the signal pending and completes it; the second
  // finds it complete.
  Signal signal;

  const int64_t iters = waitForDispatch(signal, 0, false);

  EXPECT_EQ(iters, 2);
  EXPECT_EQ(signal.value.load(), 0);
}

TEST(GpuSpinWaitTest, SpinWaitsForTheRequestedTime) {
  Signal signal;

  const Waits w = timedWaits(signal, false);

  // The tick rate is calibrated against steady_clock; allow 5% error. The
  // fastest wait should include no preemption.
  EXPECT_GE(w.min_us, 0.95 * kWaitUs);
  EXPECT_LT(w.min_us, 1.5 * kWaitUs);
  EXPECT_GT(w.min_iters, 2);
  EXPECT_EQ(signal.value.load(), 0);
}

TEST(GpuSpinWaitTest, MwaitxWaitsForTheRequestedTimeWithFewerChecks) {
  if (!cpuHasMwaitx()) {
    GTEST_SKIP() << "CPU lacks MONITORX/MWAITX";
  }
  Signal signal;
  const Waits spin = timedWaits(signal, false);

  const Waits park = timedWaits(signal, true);

  EXPECT_GE(park.min_us, 0.95 * kWaitUs);
  EXPECT_LT(park.min_us, 1.5 * kWaitUs);
  EXPECT_LT(park.min_iters, spin.min_iters);
  EXPECT_EQ(signal.value.load(), 0);
}

TEST(GpuSpinWaitTest, ValidateRejectsInvalidParams) {
  EXPECT_NO_THROW(validate(0, 1));
  EXPECT_THROW(validate(-1, 1), std::invalid_argument);
  EXPECT_THROW(
      validate(std::numeric_limits<double>::infinity(), 1),
      std::invalid_argument);
  EXPECT_THROW(validate(std::nan(""), 1), std::invalid_argument);
  EXPECT_THROW(validate(20, 0), std::invalid_argument);
}

} // namespace
} // namespace facebook::cea::chips::adsim::gpu_spin_wait

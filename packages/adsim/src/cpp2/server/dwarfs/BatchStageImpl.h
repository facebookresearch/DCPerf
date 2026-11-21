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

#pragma once

// Batch-level work for BatchQueue and BatchMerge: when a batch closes, and how
// many times a sub-kernel runs for a batch with a given number of examples.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace facebook::cea::chips::adsim::batch_stage {

/* Time from now_us until a batch opened then closes: window_us, or with
 * periodic, until the next multiple of window_us, so that every request waits
 * a uniform share of the window whatever the load.
 */
inline double closeDelayUs(double window_us, bool periodic, double now_us) {
  if (!periodic || 0 >= window_us) {
    return window_us;
  }
  return window_us - std::fmod(now_us, window_us);
}

/* A sub-kernel runs round(base + per_example * n^exponent) times for a batch
 * of n examples: base 1 and per_example 0 run it once per batch.
 */
struct Repeat {
  double base = 1.0;
  double per_example = 0.0;
  double exponent = 1.0;
};

inline void validate(const Repeat& r) {
  if (!(r.base >= 0 && r.per_example >= 0 && r.exponent >= 0)) {
    throw std::invalid_argument(
        "BatchQueue/BatchMerge: repeat base, per_example and exponent must be "
        "non-negative");
  }
}

/* Times a sub-kernel runs for a batch of n examples. */
inline int64_t repetitions(const Repeat& r, int64_t n) {
  return std::max<int64_t>(
      0,
      std::llround(
          r.base +
          r.per_example * std::pow(static_cast<double>(n), r.exponent)));
}

} // namespace facebook::cea::chips::adsim::batch_stage

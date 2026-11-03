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

// Request batching for BatchFakeIO: requests that arrive within a window form
// one batch, and the batch takes a time that grows with its size.

#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <stdexcept>
#include <utility>

#include <folly/Synchronized.h>

namespace facebook::cea::chips::adsim::batch_fake_io {

struct Params {
  double window_us; // time a batch stays open after its first request
  double base_us; // batch time independent of size
  double per_request_us; // batch time per request, before the exponent
  double exponent; // batch time grows with size^exponent
  double sigma; // lognormal noise on the batch time (median 1)
};

inline void validate(const Params& p) {
  if (!(p.window_us >= 0 && p.base_us >= 0 && p.per_request_us >= 0 &&
        p.exponent >= 0 && p.sigma >= 0)) {
    throw std::invalid_argument(
        "BatchFakeIO: window_us, base_us, per_request_us, exponent and sigma "
        "must be non-negative");
  }
}

/* Median time of a batch of n requests, in us. */
inline double batchTimeUs(const Params& p, int64_t n) {
  return p.base_us +
      p.per_request_us * std::pow(static_cast<double>(n), p.exponent);
}

/* Groups requests into batches. The first request of a batch opens it and
 * later requests join it until the opener closes it.
 */
template <typename Batch>
class Batcher {
 public:
  /* Adds a request; returns its batch and whether this request opened it. */
  std::pair<std::shared_ptr<Batch>, bool> join() {
    auto open = open_.wlock();
    const bool opened = !*open;
    if (opened) {
      *open = std::make_shared<Batch>();
    }
    ++(*open)->size;
    return {*open, opened};
  }

  /* Closes the batch so later requests start a new one; returns its size. */
  int64_t close(const std::shared_ptr<Batch>& batch) {
    auto open = open_.wlock();
    if (*open == batch) {
      open->reset();
    }
    return batch->size;
  }

 private:
  folly::Synchronized<std::shared_ptr<Batch>> open_;
};

/* Lognormal multiplier with median 1, shared by concurrent batches. */
class Noise {
 public:
  Noise(double sigma, uint64_t seed)
      : sigma_{sigma},
        state_{State{
            std::mt19937_64{seed},
            std::lognormal_distribution<double>{0.0, sigma}}} {}

  double sample() {
    if (0 == sigma_) {
      return 1.0;
    }
    auto state = state_.wlock();
    return state->dist(state->rng);
  }

 private:
  struct State {
    std::mt19937_64 rng;
    std::lognormal_distribution<double> dist;
  };
  double sigma_;
  folly::Synchronized<State> state_;
};

} // namespace facebook::cea::chips::adsim::batch_fake_io

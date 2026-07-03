// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#include <folly/dynamic.h>
#include <folly/json.h>

namespace ranking {

/**
 * PercentileSampler — inverse-CDF sampler over an arbitrary percentile
 * distribution. Replaces the original RequestSizeSampler with a more
 * general API that can load any of the 60 distributions in rpc_dist.json
 * (20 outbound methods x 3 metrics: request_sizes, response_sizes,
 * latency_us) plus the 5 inbound distributions.
 *
 * Two input shapes are supported:
 *
 *   1) Legacy single-distribution-per-file with prefixed keys, used by
 *      DriverNodeRank's --request_size_distribution flag:
 *
 *      [{"<prefix>_min": N, "<prefix>_p05": N, ..., "<prefix>_max": N}]
 *
 *      Loaded via load(json_path, field_prefix).
 *
 *   2) Bare percentile object (no prefix), used by RpcDistRegistry to
 *      consume the per-method nested objects in rpc_dist.json:
 *
 *      {"min": N, "p05": N, ..., "max": N}
 *
 *      Loaded via loadFromDynamic(obj).
 *
 * Recognized percentile keys: min, p05, p10, p15, p20, p25, p30, p35,
 * p40, p45, p50, p55, p60, p65, p70, p75, p80, p85, p90, p95, p99, max.
 * Missing keys are silently skipped (rpc_dist.json has no p99, so this
 * is normal).
 *
 * Thread-safety: load*() must complete before any sample*() call. After
 * load, sample*() is read-only on the percentile vectors but requires a
 * caller-owned RNG (typically thread-local).
 */
class PercentileSampler {
 public:
  PercentileSampler() = default;

  /**
   * Load percentile data from a JSON file with prefixed keys (legacy
   * shape used by DriverNodeRank's --request_size_distribution).
   * Returns true on success.
   */
  bool load(const std::string& json_path, const std::string& field_prefix) {
    std::ifstream ifs(json_path);
    if (!ifs.is_open()) {
      std::cerr << "PercentileSampler: cannot open " << json_path << std::endl;
      return false;
    }
    std::stringstream buf;
    buf << ifs.rdbuf();

    folly::dynamic root;
    try {
      root = folly::parseJson(buf.str());
    } catch (const std::exception& e) {
      std::cerr << "PercentileSampler: JSON parse error: " << e.what()
                << std::endl;
      return false;
    }

    const folly::dynamic* obj = nullptr;
    if (root.isArray() && root.size() > 0 && root[0].isObject()) {
      obj = &root[0];
    } else if (root.isObject()) {
      obj = &root;
    } else {
      std::cerr << "PercentileSampler: expected JSON array of objects or "
                << "single object" << std::endl;
      return false;
    }

    return loadInternal(*obj, field_prefix, /*log_path=*/json_path);
  }

  /**
   * Load percentile data from an already-parsed folly::dynamic object.
   * Keys are bare percentile names (no prefix) — for use with the
   * per-method nested objects in rpc_dist.json. Returns true if at least
   * 2 percentile points were loaded.
   */
  bool loadFromDynamic(const folly::dynamic& obj) {
    if (!obj.isObject()) {
      return false;
    }
    return loadInternal(obj, /*field_prefix=*/"", /*log_path=*/"");
  }

  /**
   * Sample a target value in bytes/units. Returns 0 if not loaded.
   * Used for size distributions (request_sizes, response_sizes).
   */
  size_t sample(std::mt19937& rng) const {
    if (probs_.empty()) return 0;
    std::uniform_real_distribution<double> u(0.0, 1.0);
    double p = u(rng);
    return static_cast<size_t>(valueAtProb(p));
  }

  /**
   * Sample a target value as int64 (latency_us values can exceed 2^31
   * — rpc_dist.json has streamData latency max=12,801,542 which fits in
   * int32, but other tail values may not in the future). Returns 0 if
   * not loaded.
   */
  int64_t sampleI64(std::mt19937& rng) const {
    if (probs_.empty()) return 0;
    std::uniform_real_distribution<double> u(0.0, 1.0);
    double p = u(rng);
    return static_cast<int64_t>(valueAtProb(p));
  }

  bool isLoaded() const { return !probs_.empty(); }
  size_t numPoints() const { return probs_.size(); }

  // Test/introspection helpers.
  int64_t minValue() const { return values_.empty() ? 0 : values_.front(); }
  int64_t maxValue() const { return values_.empty() ? 0 : values_.back(); }
  // Value at a specific probability (used by tests).
  int64_t valueAtProbability(double p) const {
    if (probs_.empty()) return 0;
    return static_cast<int64_t>(valueAtProb(p));
  }

 private:
  static const std::vector<std::pair<std::string, double>>& kPercentiles() {
    static const std::vector<std::pair<std::string, double>> kP = {
        {"min", 0.00}, {"p05", 0.05}, {"p10", 0.10}, {"p15", 0.15},
        {"p20", 0.20}, {"p25", 0.25}, {"p30", 0.30}, {"p35", 0.35},
        {"p40", 0.40}, {"p45", 0.45}, {"p50", 0.50}, {"p55", 0.55},
        {"p60", 0.60}, {"p65", 0.65}, {"p70", 0.70}, {"p75", 0.75},
        {"p80", 0.80}, {"p85", 0.85}, {"p90", 0.90}, {"p95", 0.95},
        {"p99", 0.99}, {"max", 1.00},
    };
    return kP;
  }

  bool loadInternal(
      const folly::dynamic& obj,
      const std::string& field_prefix,
      const std::string& log_path) {
    probs_.clear();
    values_.clear();
    for (const auto& [suffix, p] : kPercentiles()) {
      std::string key = field_prefix.empty()
          ? suffix
          : (field_prefix + "_" + suffix);
      auto it = obj.find(key);
      if (it == obj.items().end()) continue;
      // folly::parseJson may emit numbers as either int or double; accept both.
      int64_t v = 0;
      if (it->second.isInt()) {
        v = it->second.asInt();
      } else if (it->second.isDouble()) {
        v = static_cast<int64_t>(it->second.asDouble());
      } else {
        continue;
      }
      if (v < 0) continue;
      probs_.push_back(p);
      values_.push_back(v);
    }

    if (probs_.size() < 2) {
      std::cerr << "PercentileSampler: need at least 2 percentile points, "
                << "got " << probs_.size();
      if (!field_prefix.empty()) {
        std::cerr << " for prefix '" << field_prefix << "'";
      }
      std::cerr << std::endl;
      // Roll back partial state so isLoaded() returns false. Otherwise a
      // failed load with 1 valid percentile would silently appear loaded
      // and degenerate to a single-value distribution.
      probs_.clear();
      values_.clear();
      return false;
    }

    if (!log_path.empty()) {
      std::cerr << "PercentileSampler: loaded " << probs_.size()
                << " percentiles from " << log_path
                << " (min=" << values_.front() << ", p50="
                << valueAtProbability(0.5) << ", p99="
                << valueAtProbability(0.99)
                << ", max=" << values_.back() << ")" << std::endl;
    }
    return true;
  }

  // Linear interpolation between adjacent percentile points.
  double valueAtProb(double p) const {
    if (p <= probs_.front()) return static_cast<double>(values_.front());
    if (p >= probs_.back()) return static_cast<double>(values_.back());
    auto it = std::lower_bound(probs_.begin(), probs_.end(), p);
    size_t hi = static_cast<size_t>(it - probs_.begin());
    size_t lo = hi - 1;
    double t = (p - probs_[lo]) / (probs_[hi] - probs_[lo]);
    return static_cast<double>(values_[lo]) +
        t * (static_cast<double>(values_[hi]) -
             static_cast<double>(values_[lo]));
  }

  std::vector<double> probs_;
  std::vector<int64_t> values_;
};

} // namespace ranking

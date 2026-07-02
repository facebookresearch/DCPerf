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
 * RequestSizeSampler - inverse-CDF sampler over a percentile distribution.
 *
 * Loads a JSON file of the form:
 *   [{"<prefix>_min": N, "<prefix>_p05": N, "<prefix>_p10": N, ...,
 *     "<prefix>_p99": N, "<prefix>_max": N}]
 *
 * Each request, samples a uniform u in [0,1] and returns the size at that
 * cumulative probability via linear interpolation between adjacent percentiles.
 *
 * The keys recognised are min, p05, p10, p15, p20, p25, p30, p35, p40, p45,
 * p50, p55, p60, p65, p70, p75, p80, p85, p90, p95, p99, max.
 *
 * Thread-safety: load() must complete before sample() calls. After loading,
 * sample() is read-only on the percentile vectors but requires a per-thread
 * RNG (caller-owned).
 */
class RequestSizeSampler {
 public:
  RequestSizeSampler() = default;

  /**
   * Load percentile data from JSON file. Field names must start with the
   * given prefix, e.g. "req_size" -> req_size_min, req_size_p05, ...
   * Returns true on success.
   */
  bool load(const std::string& json_path, const std::string& field_prefix) {
    std::ifstream ifs(json_path);
    if (!ifs.is_open()) {
      std::cerr << "RequestSizeSampler: cannot open " << json_path
                << std::endl;
      return false;
    }
    std::stringstream buf;
    buf << ifs.rdbuf();

    folly::dynamic root;
    try {
      root = folly::parseJson(buf.str());
    } catch (const std::exception& e) {
      std::cerr << "RequestSizeSampler: JSON parse error: " << e.what()
                << std::endl;
      return false;
    }

    // Expect a one-element array of objects (matches Scuba/SR export format).
    const folly::dynamic* obj = nullptr;
    if (root.isArray() && root.size() > 0 && root[0].isObject()) {
      obj = &root[0];
    } else if (root.isObject()) {
      obj = &root;
    } else {
      std::cerr << "RequestSizeSampler: expected JSON array of objects or "
                << "single object" << std::endl;
      return false;
    }

    static const std::vector<std::pair<std::string, double>> kPercentiles = {
        {"min", 0.00},  {"p05", 0.05}, {"p10", 0.10}, {"p15", 0.15},
        {"p20", 0.20},  {"p25", 0.25}, {"p30", 0.30}, {"p35", 0.35},
        {"p40", 0.40},  {"p45", 0.45}, {"p50", 0.50}, {"p55", 0.55},
        {"p60", 0.60},  {"p65", 0.65}, {"p70", 0.70}, {"p75", 0.75},
        {"p80", 0.80},  {"p85", 0.85}, {"p90", 0.90}, {"p95", 0.95},
        {"p99", 0.99},  {"max", 1.00},
    };

    probs_.clear();
    sizes_.clear();
    for (const auto& [suffix, p] : kPercentiles) {
      std::string key = field_prefix + "_" + suffix;
      auto it = obj->find(key);
      if (it == obj->items().end()) continue;
      // Accept both int and double (folly::parseJson may emit either).
      int64_t v = 0;
      if (it->second.isInt()) {
        v = it->second.asInt();
      } else if (it->second.isDouble()) {
        v = static_cast<int64_t>(it->second.asDouble());
      } else {
        std::cerr << "RequestSizeSampler: skipping " << key
                  << " (unexpected value type)" << std::endl;
        continue;
      }
      if (v < 0) continue;
      probs_.push_back(p);
      sizes_.push_back(static_cast<size_t>(v));
    }

    if (probs_.size() < 2) {
      std::cerr << "RequestSizeSampler: need at least 2 percentile points, "
                << "got " << probs_.size() << " for prefix '" << field_prefix
                << "'" << std::endl;
      return false;
    }

    std::cerr << "RequestSizeSampler: loaded " << probs_.size()
              << " percentiles from " << json_path
              << " (min=" << sizes_.front() << ", p50="
              << sizeAtProb(0.5) << ", p99=" << sizeAtProb(0.99)
              << ", max=" << sizes_.back() << ")" << std::endl;
    return true;
  }

  /**
   * Sample a target size in bytes. Returns 0 if not loaded.
   */
  size_t sample(std::mt19937& rng) const {
    if (probs_.empty()) return 0;
    std::uniform_real_distribution<double> u(0.0, 1.0);
    double p = u(rng);
    return sizeAtProb(p);
  }

  bool isLoaded() const { return !probs_.empty(); }

 private:
  // Linear interpolation between adjacent percentile points.
  size_t sizeAtProb(double p) const {
    if (p <= probs_.front()) return sizes_.front();
    if (p >= probs_.back()) return sizes_.back();
    auto it = std::lower_bound(probs_.begin(), probs_.end(), p);
    size_t hi = static_cast<size_t>(it - probs_.begin());
    size_t lo = hi - 1;
    double t =
        (p - probs_[lo]) / (probs_[hi] - probs_[lo]);
    double interp =
        static_cast<double>(sizes_[lo]) +
        t * (static_cast<double>(sizes_[hi]) -
             static_cast<double>(sizes_[lo]));
    return static_cast<size_t>(interp);
  }

  std::vector<double> probs_;
  std::vector<size_t> sizes_;
};

} // namespace ranking

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

#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

#include <folly/dynamic.h>
#include <folly/json.h>

#include "PercentileSampler.h"

namespace ranking {

// Twenty outbound RPC methods. The order MUST match
// ~/feedsim_v2/profiles/rpc_dist.json so kPerSessionCounts and kMethodNames
// stay aligned. Keep in sync with mock_services/MockService.thrift.
//
// IMPORTANT: do not reorder. The integer values are persisted in
// dispatchByEnum() and any future serialization (e.g. logging tags).
enum class MethodIdx : uint8_t {
  mcGet = 0,
  mcLeaseGet = 1,
  mcSet = 2,
  fetchTopKEntitiesRequest = 3,
  getActionStreamsRequestCompressed2 = 4,
  getObjectsFromQueries = 5,
  getSerializedObjects = 6,
  getStatus = 7,
  runFullyRemotePrediction = 8,
  getActionStreamsCompressed2 = 9,
  runModelMethod = 10,
  edsMultiGet = 11,
  fetchCandidateScoreRequest = 12,
  mcLeaseSet = 13,
  prefixScan = 14,
  fetchEntityFeatures = 15,
  getUserConsents = 16,
  fciGet = 17,
  multiget = 18,
  FbkeyPointGetRequest = 19,
  COUNT,
};

constexpr size_t kNumMethods = static_cast<size_t>(MethodIdx::COUNT);

// Method name strings, indexed by MethodIdx. Used for JSON lookup,
// logging, and string-keyed test introspection.
inline const std::array<const char*, kNumMethods>& methodNames() {
  static const std::array<const char*, kNumMethods> kNames = {
      "mcGet",
      "mcLeaseGet",
      "mcSet",
      "fetchTopKEntitiesRequest",
      "getActionStreamsRequestCompressed2",
      "getObjectsFromQueries",
      "getSerializedObjects",
      "getStatus",
      "runFullyRemotePrediction",
      "getActionStreamsCompressed2",
      "runModelMethod",
      "edsMultiGet",
      "fetchCandidateScoreRequest",
      "mcLeaseSet",
      "prefixScan",
      "fetchEntityFeatures",
      "getUserConsents",
      "fciGet",
      "multiget",
      "FbkeyPointGetRequest",
  };
  return kNames;
}

// Per-session call counts at the production multifeed_aggregator profile,
// computed as outbound[method].total_weighted_count divided by the inbound
// session count (createAndPrimeSession.total_weighted_count =
// 209,362,757,754). See ~/feedsim_v2/docs/phase5_researcher_notes.md §4.
//
// Hard-coded so a missing or stale rpc_dist.json cannot silently change the
// fanout. Sum across all 20 methods is ~3,742 RPCs/session at scale=1.0;
// the recommended default --rpc_fanout_scale=0.025 yields ~94 RPCs/session.
inline const std::array<double, kNumMethods>& perSessionCounts() {
  static const std::array<double, kNumMethods> kCounts = {
      1377.07, // mcGet
      505.26, // mcLeaseGet
      383.94, // mcSet
      229.32, // fetchTopKEntitiesRequest
      187.58, // getActionStreamsRequestCompressed2
      164.43, // getObjectsFromQueries
      135.84, // getSerializedObjects
      123.78, // getStatus
      122.30, // runFullyRemotePrediction
      95.91, // getActionStreamsCompressed2
      82.47, // runModelMethod
      58.28, // edsMultiGet
      50.64, // fetchCandidateScoreRequest
      45.12, // mcLeaseSet
      44.35, // prefixScan
      40.15, // fetchEntityFeatures
      37.65, // getUserConsents
      23.43, // fciGet
      21.08, // multiget
      13.59, // FbkeyPointGetRequest
  };
  return kCounts;
}

/**
 * RpcDistRegistry — loads rpc_dist.json once at startup and exposes the 60
 * outbound percentile samplers (20 methods x {request_size, response_size,
 * latency_us}) plus the 15 inbound samplers (5 methods x 3 metrics).
 *
 * Thread-safety: load() must complete before any sampler accessor is
 * called. Once loaded, accessors and the underlying samplers are
 * read-only and safe to share across threads (each thread brings its own
 * RNG to sample()).
 */
class RpcDistRegistry {
 public:
  RpcDistRegistry() = default;

  /**
   * Parse rpc_dist.json from `json_path` and populate the 60 outbound
   * samplers. Inbound samplers are populated when the corresponding keys
   * exist. Returns true if all 20 outbound methods loaded all 3 metrics
   * successfully; logs and returns false otherwise.
   */
  bool load(const std::string& json_path) {
    std::ifstream ifs(json_path);
    if (!ifs.is_open()) {
      std::cerr << "RpcDistRegistry: cannot open " << json_path << std::endl;
      return false;
    }
    std::stringstream buf;
    buf << ifs.rdbuf();

    folly::dynamic root;
    try {
      root = folly::parseJson(buf.str());
    } catch (const std::exception& e) {
      std::cerr << "RpcDistRegistry: JSON parse error: " << e.what()
                << std::endl;
      return false;
    }
    if (!root.isObject()) {
      std::cerr << "RpcDistRegistry: top-level JSON must be an object"
                << std::endl;
      return false;
    }

    auto outbound_it = root.find("outbound");
    if (outbound_it == root.items().end() || !outbound_it->second.isObject()) {
      std::cerr << "RpcDistRegistry: missing 'outbound' object" << std::endl;
      return false;
    }
    const folly::dynamic& outbound = outbound_it->second;

    bool all_ok = true;
    for (size_t i = 0; i < kNumMethods; ++i) {
      const char* name = methodNames()[i];
      auto method_it = outbound.find(name);
      if (method_it == outbound.items().end() ||
          !method_it->second.isObject()) {
        std::cerr << "RpcDistRegistry: missing outbound method '" << name
                  << "'" << std::endl;
        all_ok = false;
        continue;
      }
      const folly::dynamic& method_obj = method_it->second;
      all_ok &= loadMetric(method_obj, "request_sizes", req_[i], name);
      all_ok &= loadMetric(method_obj, "response_sizes", resp_[i], name);
      all_ok &= loadMetric(method_obj, "latency_us", lat_[i], name);
    }

    // Inbound section is optional for fanout but useful for future tooling
    // (e.g., DriverNode shaping inbound payloads). We do not require it.
    auto inbound_it = root.find("inbound");
    if (inbound_it != root.items().end() && inbound_it->second.isObject()) {
      // No-op for now; reserved for future use. Silently parse to validate.
      (void)inbound_it;
    }

    if (all_ok) {
      std::cerr << "RpcDistRegistry: loaded 20 outbound methods x 3 metrics"
                << " from " << json_path << std::endl;
    }
    return all_ok;
  }

  // Per-method accessors by enum.
  const PercentileSampler& requestSize(MethodIdx m) const {
    return req_[static_cast<size_t>(m)];
  }
  const PercentileSampler& responseSize(MethodIdx m) const {
    return resp_[static_cast<size_t>(m)];
  }
  const PercentileSampler& latencyUs(MethodIdx m) const {
    return lat_[static_cast<size_t>(m)];
  }

  // String-keyed convenience for tests and introspection. Returns a
  // pointer to the empty fallback sampler if the name is unknown.
  const PercentileSampler& requestSize(std::string_view method_name) const {
    return samplerByName(method_name, req_);
  }
  const PercentileSampler& responseSize(std::string_view method_name) const {
    return samplerByName(method_name, resp_);
  }
  const PercentileSampler& latencyUs(std::string_view method_name) const {
    return samplerByName(method_name, lat_);
  }

  // True if every outbound metric loaded successfully.
  bool isFullyLoaded() const {
    for (size_t i = 0; i < kNumMethods; ++i) {
      if (!req_[i].isLoaded() || !resp_[i].isLoaded() ||
          !lat_[i].isLoaded()) {
        return false;
      }
    }
    return true;
  }

 private:
  static bool loadMetric(
      const folly::dynamic& method_obj,
      const char* metric_key,
      PercentileSampler& out,
      const char* method_name) {
    auto it = method_obj.find(metric_key);
    if (it == method_obj.items().end() || !it->second.isObject()) {
      std::cerr << "RpcDistRegistry: method '" << method_name
                << "' missing '" << metric_key << "'" << std::endl;
      return false;
    }
    if (!out.loadFromDynamic(it->second)) {
      std::cerr << "RpcDistRegistry: method '" << method_name
                << "' failed to parse '" << metric_key << "'" << std::endl;
      return false;
    }
    return true;
  }

  const PercentileSampler& samplerByName(
      std::string_view method_name,
      const std::array<PercentileSampler, kNumMethods>& arr) const {
    const auto& names = methodNames();
    for (size_t i = 0; i < kNumMethods; ++i) {
      if (method_name == names[i]) {
        return arr[i];
      }
    }
    return empty_;
  }

  std::array<PercentileSampler, kNumMethods> req_;
  std::array<PercentileSampler, kNumMethods> resp_;
  std::array<PercentileSampler, kNumMethods> lat_;
  PercentileSampler empty_;
};

} // namespace ranking

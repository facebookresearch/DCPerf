// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#pragma once

#include <inttypes.h>

#include <algorithm>
#include <map>
#include <set>
#include <vector>

#include "oldisim/Log.h"
#include "oldisim/LogHistogramSampler.h"
#include "oldisim/Query.h"
#include "oldisim/Response.h"
#include "oldisim/Util.h"

namespace oldisim {

class ChildConnectionStats {
 public:
  explicit ChildConnectionStats(const std::set<uint32_t>& query_types) {
    const int kHistogramBins = 1000;
    for (auto type : query_types) {
      // query_samplers_.emplace(type, std::unique_ptr<LogHistogramSampler>(new
      // LogHistogramSampler(kHistogramBins)));
      query_samplers_.insert(
          std::make_pair(type, LogHistogramSampler(kHistogramBins)));
      query_processing_time_samplers_.insert(
          std::make_pair(type, LogHistogramSampler(kHistogramBins)));
      tx_bytes_[type] = 0;
      rx_bytes_[type] = 0;
      query_counts_[type] = 0;
      dropped_requests_[type] = 0;
    }
    start_time_ = GetTimeAccurateNano();
  }

  uint64_t start_time_;
  uint64_t end_time_;
  std::map<uint32_t, LogHistogramSampler> query_samplers_;
  std::map<uint32_t, LogHistogramSampler> query_processing_time_samplers_;
  std::map<uint32_t, uint64_t> tx_bytes_;
  std::map<uint32_t, uint64_t> rx_bytes_;
  std::map<uint32_t, uint64_t> query_counts_;
  std::map<uint32_t, uint64_t> dropped_requests_;

  void LogRequest(const Query& request) {
    assert(tx_bytes_.count(request.GetType()) > 0);
    assert(query_counts_.count(request.GetType()) > 0);

    tx_bytes_.at(request.GetType()) += request.GetQueryPacketSize();
    query_counts_.at(request.GetType())++;
  }

  void LogResponse(const Query& originating_request, const Response& response) {
    assert(query_samplers_.count(originating_request.GetType()) > 0);
    assert(query_processing_time_samplers_.count(
               originating_request.GetType()) > 0);
    assert(tx_bytes_.count(response.GetType()) > 0);

    query_samplers_.at(originating_request.GetType())
        .sample(originating_request.Time());
    query_processing_time_samplers_.at(originating_request.GetType())
        .sample(response.GetProcessingTime());
    rx_bytes_.at(response.GetType()) += response.GetResponsePacketSize();
  }

  void LogDroppedRequest(uint32_t request_type) {
    assert(dropped_requests_.count(request_type) > 0);
    dropped_requests_.at(request_type)++;
  }

  void Accumulate(const ChildConnectionStats& cs) {
    assert(cs.query_samplers_.size() == query_samplers_.size());
    assert(cs.query_processing_time_samplers_.size() ==
           query_processing_time_samplers_.size());
    assert(cs.tx_bytes_.size() == tx_bytes_.size());
    assert(cs.rx_bytes_.size() == rx_bytes_.size());
    assert(cs.query_counts_.size() == query_counts_.size());
    assert(cs.dropped_requests_.size() == dropped_requests_.size());

    for (const auto& sampler : cs.query_samplers_) {
      query_samplers_.at(sampler.first).accumulate(sampler.second);
    }

    for (const auto& sampler : cs.query_processing_time_samplers_) {
      query_processing_time_samplers_.at(sampler.first)
          .accumulate(sampler.second);
    }

    for (const auto& stat : cs.tx_bytes_) {
      tx_bytes_[stat.first] += stat.second;
    }

    for (const auto& stat : cs.rx_bytes_) {
      rx_bytes_[stat.first] += stat.second;
    }

    for (const auto& stat : cs.query_counts_) {
      query_counts_[stat.first] += stat.second;
    }

    for (const auto& stat : cs.dropped_requests_) {
      dropped_requests_[stat.first] += stat.second;
    }
  }

  void Reset() {
    for (const auto& stat : query_samplers_) {
      query_samplers_.at(stat.first).Reset();
      query_processing_time_samplers_.at(stat.first).Reset();
      tx_bytes_[stat.first] = 0;
      rx_bytes_[stat.first] = 0;
      query_counts_[stat.first] = 0;
      dropped_requests_[stat.first] = 0;
    }
    start_time_ = GetTimeAccurateNano();
  }
};
}  // namespace oldisim

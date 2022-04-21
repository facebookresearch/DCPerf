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

#ifndef OLDISIM_LEAF_NODE_STATS_H
#define OLDISIM_LEAF_NODE_STATS_H

#include <assert.h>
#include <stdint.h>

#include <map>
#include <set>

#include "oldisim/Log.h"
#include "oldisim/LogHistogramSampler.h"
#include "oldisim/QueryContext.h"
#include "oldisim/Response.h"

namespace oldisim {

class LeafNodeStats {
 public:
  explicit LeafNodeStats(const std::set<uint32_t>& query_types) {
    const int kHistogramBins = 200;
    for (auto type : query_types) {
      tx_bytes_[type] = 0;
      rx_bytes_[type] = 0;
      query_counts_[type] = 0;
      response_counts_[type] = 0;
      processing_time_samplers_.insert(
          std::make_pair(type, LogHistogramSampler(kHistogramBins)));
    }
  }

  std::map<uint32_t, uint64_t> tx_bytes_;
  std::map<uint32_t, uint64_t> rx_bytes_;
  std::map<uint32_t, uint64_t> query_counts_;
  std::map<uint32_t, uint64_t> response_counts_;
  std::map<uint32_t, LogHistogramSampler> processing_time_samplers_;

  void LogQuery(const QueryContext& query) {
    assert(rx_bytes_.count(query.type) > 0);
    assert(query_counts_.count(query.type) > 0);
    rx_bytes_.at(query.type) += query.packet_length;
    query_counts_.at(query.type)++;
  }

  void LogResponse(const Response& response) {
    assert(tx_bytes_.count(response.GetType()) > 0);
    assert(response_counts_.count(response.GetType()) > 0);
    assert(processing_time_samplers_.count(response.GetType()) > 0);
    tx_bytes_.at(response.GetType()) += response.GetResponsePacketSize();
    response_counts_.at(response.GetType())++;
    processing_time_samplers_.at(response.GetType())
        .sample(response.GetProcessingTime());
  }

  void Accumulate(const LeafNodeStats& cs) {
    assert(cs.tx_bytes_.size() == tx_bytes_.size());
    assert(cs.rx_bytes_.size() == rx_bytes_.size());
    assert(cs.query_counts_.size() == query_counts_.size());
    assert(cs.processing_time_samplers_.size() ==
           processing_time_samplers_.size());

    for (auto& stat : cs.tx_bytes_) {
      tx_bytes_.at(stat.first) += stat.second;
    }

    for (auto& stat : cs.rx_bytes_) {
      rx_bytes_.at(stat.first) += stat.second;
    }

    for (auto& stat : cs.query_counts_) {
      query_counts_.at(stat.first) += stat.second;
    }

    for (auto& stat : cs.response_counts_) {
      response_counts_.at(stat.first) += stat.second;
    }

    for (const auto& sampler : cs.processing_time_samplers_) {
      processing_time_samplers_.at(sampler.first).accumulate(sampler.second);
    }
  }

  void Reset() {
    for (auto& stat : tx_bytes_) {
      tx_bytes_[stat.first] = 0;
      rx_bytes_[stat.first] = 0;
      query_counts_[stat.first] = 0;
      response_counts_[stat.first] = 0;
      processing_time_samplers_.at(stat.first).Reset();
    }
  }
};
}  // namespace oldisim

#endif  // OLDISIM_LEAF_NODE_STATS_H

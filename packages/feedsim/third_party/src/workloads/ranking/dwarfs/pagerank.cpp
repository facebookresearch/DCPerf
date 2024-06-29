// Copyright (c) 2015, The Regents of the University of California (Regents).
// All Rights Reserved.

// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
// 1. Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
// 2. Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
// 3. Neither the name of the Regents nor the
//    names of its contributors may be used to endorse or promote products
//    derived from this software without specific prior written permission.

// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL REGENTS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
// OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
// EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "pagerank.h"
#include <algorithm>
#include <iostream>
#include <random>
#include <string>

#include <gapbs/src/benchmark.h>
#include <gapbs/src/command_line.h>

namespace ranking {
namespace dwarfs {

constexpr auto kPageRankTrials = 10;
constexpr auto kPageRankTolerance = 1e-4;
constexpr auto kPageRankMaxIters = 20;

struct PageRankParams::Impl {
 public:
  explicit Impl(std::unique_ptr<CLPageRankDummy> cli) : cli_{std::move(cli)} {
    cli_->ParseArgs();
    builder_ = std::make_unique<Builder>(*cli_);
  }
  CSRGraph<int32_t> makeGraph() {
    return std::move(builder_->MakeGraph());
  }

 private:
  std::unique_ptr<Builder> builder_;
  std::unique_ptr<CLPageRankDummy> cli_;
};

PageRankParams::PageRankParams(int scale, int degrees)
    : scale_(scale), degrees_(degrees) {
  auto scale_str = std::to_string(scale_);
  std::unique_ptr<CLPageRankDummy> cli{new CLPageRankDummy(
      scale,
      degrees,
      true,
      kPageRankTrials,
      kPageRankTolerance,
      kPageRankMaxIters)};
  pimpl = std::make_unique<Impl>(std::move(cli));
}

PageRankParams::~PageRankParams() = default;

CSRGraph<int32_t> PageRankParams::buildGraph() {
  return pimpl->makeGraph();
}

PageRank::PageRank(CSRGraph<int32_t> graph, int num_pvectors_entries)
    : graph_(std::move(graph)), num_pvectors_entries_(num_pvectors_entries) {
  const float init_score = 1.0f / graph_.num_nodes();
  for (int i = 0; i < num_pvectors_entries; i++) {
    pvector<float> scores{graph_.num_nodes(), init_score};
    pvector<float> outgoing_contrib{graph_.num_nodes()};

    scores_pvectors_map_[i] = std::move(scores);
    outgoing_pvectors_map_[i] = std::move(outgoing_contrib);
  }
}

/** PageRank implementation taken from
 * http://gap.cs.berkeley.edu/benchmark.html
 */
int PageRank::rank(
    int thread_id,
    int max_iters,
    double epsilon,
    int rank_trials,
    int subset) {
  std::vector<int> sizes;
  const int64_t num_nodes = subset > 0
      ? std::min(static_cast<int64_t>(subset), graph_.num_nodes())
      : graph_.num_nodes();

  std::uniform_int_distribution<int64_t> u_dist{
      0,
      num_nodes < graph_.num_nodes() ? graph_.num_nodes() - num_nodes
                                     : graph_.num_nodes()};
  std::random_device rd;
  std::mt19937 gen(rd());
  NodeID start = u_dist(gen);

  const auto split_size = std::max(num_pvectors_entries_, 1);
  std::uniform_int_distribution<int64_t> split_dist{
      0, graph_.num_nodes() / split_size - 1};
  std::mt19937 split_gen(rd());
  NodeID split_start = split_dist(split_gen);
  NodeID split_end = split_start + (graph_.num_nodes() / split_size) - 1;

  for (int t = 0; t < rank_trials; t++) {
    const float base_score = (1.0f - kDamp) / graph_.num_nodes();
    pvector<float>& scores = scores_pvectors_map_[thread_id];
    pvector<float>& outgoing_contrib = outgoing_pvectors_map_[thread_id];
    int iter;
    for (iter = 0; iter < max_iters; iter++) {
      double error = 0;

      // #pragma omp parallel for
      for (NodeID n = split_start; n < split_end; n++) {
        outgoing_contrib[n] = scores[n] / graph_.out_degree(n);
      }

      // #pragma omp parallel for reduction(+ : error) schedule(dynamic, 64)
      for (NodeID u = start; u < start + num_nodes; u++) {
        float incoming_total = 0;
        for (NodeID v : graph_.in_neigh(u)) {
          incoming_total += outgoing_contrib[v];
        }
        float old_score = scores[u];
        scores[u] = base_score + kDamp * incoming_total;
        error += std::fabs(scores[u] - old_score);
      }
      if (error < epsilon) {
        break;
      }
    }
    sizes.push_back(scores.size());
  }
  // Dummy-value
  return sizes.size();
}

} // namespace dwarfs
} // namespace ranking

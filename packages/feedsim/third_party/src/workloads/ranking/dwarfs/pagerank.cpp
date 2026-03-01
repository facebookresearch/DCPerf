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
#include <fstream>
#include <iostream>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

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
CSRGraph<int32_t> PageRankParams::makeGraphCopy(
    const CSRGraph<int32_t>& original) {
  // Create a deep copy of the graph
  const int64_t num_nodes = original.num_nodes();
  const int64_t num_edges = original.num_edges();

  // Allocate memory for the new graph's index and neighbors arrays
  int32_t** out_index = new int32_t*[num_nodes + 1];
  int32_t* out_neighbors = new int32_t[num_edges];
  auto is_directed = original.directed();

  // Copy all outgoing neighbors data efficiently
  std::copy(
      original.out_neigh(0).begin(),
      original.out_neigh(num_nodes - 1).end(),
      out_neighbors);
// Set up index pointers for each node
#pragma omp parallel for
  for (int64_t n = 0; n < num_nodes; n++) {
    // TODO: check this is correct
    out_index[n] = out_neighbors +
        (original.out_neigh(n).begin() - original.out_neigh(0).begin());
  }
  // Set the last index pointer
  out_index[num_nodes] = out_neighbors + num_edges;

  // If the graph is directed and has incoming edges, copy those too
  if (is_directed) {
    int32_t** in_index = new int32_t*[num_nodes + 1];
    int32_t* in_neighbors = new int32_t[num_edges];

    // Copy all incoming neighbors data efficiently
    std::copy(
        original.in_neigh(0).begin(),
        original.in_neigh(num_nodes - 1).end(),
        in_neighbors);

// Set up index pointers for each node
#pragma omp parallel for
    for (int64_t n = 0; n < num_nodes; n++) {
      in_index[n] = in_neighbors +
          (original.in_neigh(n).begin() - original.in_neigh(0).begin());
    }
    // Set the last index pointer
    in_index[num_nodes] = in_neighbors + num_edges;

    return CSRGraph<int32_t>(
        num_nodes, out_index, out_neighbors, in_index, in_neighbors);
  } else {
    return CSRGraph<int32_t>(num_nodes, out_index, out_neighbors);
  }
}

void PageRankParams::storeGraphToFile(
    const CSRGraph<int32_t>& original,
    const std::string& filePath) {
  std::ofstream outFile(filePath, std::ios::binary);
  if (!outFile.is_open()) {
    throw std::runtime_error("Unable to open file for writing: " + filePath);
  }

  // Write basic graph properties
  bool is_directed = original.directed();
  int64_t num_nodes = original.num_nodes();
  int64_t num_edges = original.num_edges();

  outFile.write(
      reinterpret_cast<const char*>(&is_directed), sizeof(is_directed));
  outFile.write(reinterpret_cast<const char*>(&num_nodes), sizeof(num_nodes));
  outFile.write(reinterpret_cast<const char*>(&num_edges), sizeof(num_edges));

  // Write out_index array (size: num_nodes + 1 pointers, but we store offsets)
  std::vector<int64_t> out_offsets(num_nodes + 1);
  for (int64_t n = 0; n <= num_nodes; n++) {
    if (n == 0) {
      out_offsets[n] = 0;
    } else if (n == num_nodes) {
      out_offsets[n] = num_edges;
    } else {
      out_offsets[n] =
          original.out_neigh(n).begin() - original.out_neigh(0).begin();
    }
  }
  outFile.write(
      reinterpret_cast<const char*>(out_offsets.data()),
      (num_nodes + 1) * sizeof(int64_t));

  // Write out_neighbors array
  outFile.write(
      reinterpret_cast<const char*>(original.out_neigh(0).begin()),
      num_edges * sizeof(int32_t));

  // If directed, write incoming edges data
  if (is_directed) {
    // Write in_index array (as offsets)
    std::vector<int64_t> in_offsets(num_nodes + 1);
    for (int64_t n = 0; n <= num_nodes; n++) {
      if (n == 0) {
        in_offsets[n] = 0;
      } else if (n == num_nodes) {
        in_offsets[n] = num_edges;
      } else {
        in_offsets[n] =
            original.in_neigh(n).begin() - original.in_neigh(0).begin();
      }
    }
    outFile.write(
        reinterpret_cast<const char*>(in_offsets.data()),
        (num_nodes + 1) * sizeof(int64_t));

    // Write in_neighbors array
    outFile.write(
        reinterpret_cast<const char*>(original.in_neigh(0).begin()),
        num_edges * sizeof(int32_t));
  }

  outFile.close();
}

CSRGraph<int32_t> PageRankParams::loadGraphFromFile(
    const std::string& filePath) {
  std::ifstream inFile(filePath, std::ios::binary);
  if (!inFile.is_open()) {
    throw std::runtime_error("Unable to open file for reading: " + filePath);
  }

  // Read basic graph properties
  bool is_directed;
  int64_t num_nodes;
  int64_t num_edges;

  inFile.read(reinterpret_cast<char*>(&is_directed), sizeof(is_directed));
  inFile.read(reinterpret_cast<char*>(&num_nodes), sizeof(num_nodes));
  inFile.read(reinterpret_cast<char*>(&num_edges), sizeof(num_edges));

  // Read out_index offsets and convert to pointers
  std::vector<int64_t> out_offsets(num_nodes + 1);
  inFile.read(
      reinterpret_cast<char*>(out_offsets.data()),
      (num_nodes + 1) * sizeof(int64_t));

  // Allocate and read out_neighbors array
  int32_t* out_neighbors = new int32_t[num_edges];
  inFile.read(
      reinterpret_cast<char*>(out_neighbors), num_edges * sizeof(int32_t));

  // Create out_index pointer array
  int32_t** out_index = new int32_t*[num_nodes + 1];
#pragma omp parallel for
  for (int64_t n = 0; n <= num_nodes; n++) {
    out_index[n] = out_neighbors + out_offsets[n];
  }

  if (is_directed) {
    // Read in_index offsets and convert to pointers
    std::vector<int64_t> in_offsets(num_nodes + 1);
    inFile.read(
        reinterpret_cast<char*>(in_offsets.data()),
        (num_nodes + 1) * sizeof(int64_t));

    // Allocate and read in_neighbors array
    int32_t* in_neighbors = new int32_t[num_edges];
    inFile.read(
        reinterpret_cast<char*>(in_neighbors), num_edges * sizeof(int32_t));

    // Create in_index pointer array
    int32_t** in_index = new int32_t*[num_nodes + 1];
#pragma omp parallel for
    for (int64_t n = 0; n <= num_nodes; n++) {
      in_index[n] = in_neighbors + in_offsets[n];
    }

    inFile.close();
    return CSRGraph<int32_t>(
        num_nodes, out_index, out_neighbors, in_index, in_neighbors);
  } else {
    inFile.close();
    return CSRGraph<int32_t>(num_nodes, out_index, out_neighbors);
  }
}

PageRank::PageRank(
    CSRGraph<int32_t> graph,
    int num_pvectors_entries,
    unsigned seed)
    : graph_(std::move(graph)),
      num_pvectors_entries_(num_pvectors_entries),
      seed_(seed) {
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
  // Use the seed passed to the constructor
  unsigned local_seed = seed_ != 0
      ? seed_ + thread_id
      : std::chrono::system_clock::now().time_since_epoch().count();
  std::mt19937 gen(local_seed);
  NodeID start = u_dist(gen);

  const auto split_size = std::max(num_pvectors_entries_, 1);
  std::uniform_int_distribution<int64_t> split_dist{
      0, graph_.num_nodes() / split_size - 1};
  std::mt19937 split_gen(local_seed + 1);
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

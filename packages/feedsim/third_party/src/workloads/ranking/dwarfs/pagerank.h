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

#ifndef PAGERANK_H
#define PAGERANK_H

#include <cstdint>
#include <memory>

#include <folly/container/F14Map.h>
#include <gapbs/src/graph.h>
#include <gapbs/src/pvector.h>

namespace ranking {
namespace dwarfs {

class PageRankParams {
 public:
  explicit PageRankParams(int scale, int degrees);
  ~PageRankParams();

  CSRGraph<int32_t> buildGraph();

 private:
  struct Impl;
  std::unique_ptr<Impl> pimpl;

  int scale_;
  int degrees_;
};

class PageRank {
 public:
  constexpr static const float kDamp = 0.85;

  explicit PageRank(CSRGraph<int32_t> graph, int num_pvectors_entries);

  int rank(
      int thread_id,
      int max_iters,
      double epsilon,
      int rank_trials,
      int subset);

 private:
  CSRGraph<int32_t> graph_;
  int num_pvectors_entries_;
  folly::F14FastMap<int, pvector<float>> scores_pvectors_map_;
  folly::F14FastMap<int, pvector<float>> outgoing_pvectors_map_;
};

} // namespace dwarfs
} // namespace ranking

#endif // PAGERANK_H

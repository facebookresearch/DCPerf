// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "FeatureExtractorSuite.h"

#include <algorithm>
#include <iostream>
#include <random>

#include "generated/extractor_helpers.h"

void FeatureExtractorSuite::addExtractor(
    std::unique_ptr<FeatureExtractorBase> extractor) {
  extractors_.push_back(std::move(extractor));
}

void FeatureExtractorSuite::initializeAll(int complexity, int seed) {
  for (size_t i = 0; i < extractors_.size(); ++i) {
    extractors_[i]->initialize(complexity, seed + static_cast<int>(i));
    std::cout << "  Initialized extractor: " << extractors_[i]->name()
              << " (complexity=" << complexity << ")" << std::endl;
  }
}

void FeatureExtractorSuite::runAll(
    const std::vector<float>& input_dense,
    const std::vector<int64_t>& input_sparse) {
  if (extractors_.empty()) {
    return;
  }

  // First extractor reads from input, writes to buf_a
  dense_buf_a_.clear();
  sparse_buf_a_.clear();
  extractors_[0]->extract(
      input_dense, input_sparse, dense_buf_a_, sparse_buf_a_);

  // Chain remaining extractors, alternating between buf_a and buf_b
  for (size_t i = 1; i < extractors_.size(); ++i) {
    if (i % 2 == 1) {
      dense_buf_b_.clear();
      sparse_buf_b_.clear();
      extractors_[i]->extract(
          dense_buf_a_, sparse_buf_a_, dense_buf_b_, sparse_buf_b_);
    } else {
      dense_buf_a_.clear();
      sparse_buf_a_.clear();
      extractors_[i]->extract(
          dense_buf_b_, sparse_buf_b_, dense_buf_a_, sparse_buf_a_);
    }
  }
}

void FeatureExtractorSuite::runRandomSubset(
    int count,
    const std::vector<float>& input_dense,
    const std::vector<int64_t>& input_sparse,
    std::default_random_engine& rng) {
  if (extractors_.empty() || count <= 0) {
    return;
  }

  int pool_size = static_cast<int>(extractors_.size());
  std::uniform_int_distribution<int> dist(0, pool_size - 1);

  // First extractor reads from input
  int idx = dist(rng);
  dense_buf_a_.clear();
  sparse_buf_a_.clear();
  extractors_[idx]->extract(
      input_dense, input_sparse, dense_buf_a_, sparse_buf_a_);

  // Remaining extractors chain alternating buffers
  for (int i = 1; i < count; ++i) {
    idx = dist(rng);
    if (i % 2 == 1) {
      dense_buf_b_.clear();
      sparse_buf_b_.clear();
      extractors_[idx]->extract(
          dense_buf_a_, sparse_buf_a_, dense_buf_b_, sparse_buf_b_);
    } else {
      dense_buf_a_.clear();
      sparse_buf_a_.clear();
      extractors_[idx]->extract(
          dense_buf_b_, sparse_buf_b_, dense_buf_a_, sparse_buf_a_);
    }
  }
}

size_t FeatureExtractorSuite::size() const {
  return extractors_.size();
}

void FeatureExtractorSuite::initializeFlatDispatch(int seed) {
  using namespace dcperf::feature_extractors::generated;
  getAllCopyFunctions(flat_copies_);
  std::mt19937 rng(seed);
  std::shuffle(flat_copies_.begin(), flat_copies_.end(), rng);
  flat_pos_ = 0;

  // Initialize shared context data. Use unique_ptr so re-invoking
  // initializeFlatDispatch() cleanly replaces the previous buffer, and
  // destruction of the suite frees it.
  flat_struct_size_ = 512;
  flat_struct_data_ = std::make_unique<float[]>(flat_struct_size_);
  std::mt19937 data_rng(seed + 42);
  std::uniform_real_distribution<float> dist(0.0f, 1.0f);
  for (int i = 0; i < flat_struct_size_; ++i)
    flat_struct_data_[i] = dist(data_rng);

  for (int t = 0; t < 4; ++t)
    for (int i = 0; i < 1024; ++i)
      flat_tables_[t][data_rng()] = dist(data_rng);

  for (int t = 0; t < 4; ++t)
    flat_hash_tables_[t].populate(64, seed + t + 100);

  flat_features_.resize(100);
  for (int i = 0; i < 100; ++i) {
    flat_features_[i].raw_feature_index = i % 50;
    flat_features_[i].is_indexed = true;
    flat_features_[i].use_all_indexes = true;
  }

  std::cout << "  Flat dispatch initialized: " << flat_copies_.size()
            << " copy functions shuffled" << std::endl;
}

void FeatureExtractorSuite::runFlatExtractors(
    int count,
    const std::vector<float>& input_dense,
    const std::vector<int64_t>& input_sparse,
    const uint8_t* story_content,
    int story_content_length) {
  using namespace dcperf::feature_extractors::generated;
  if (flat_copies_.empty()) return;

  // Per-call mutable state. CopyContext fields the generated extractors
  // mutate (example via emplace_back, structData via in-place
  // arithmetic) MUST NOT alias across concurrent invocations on the
  // same suite: concurrent emplace_back on a shared
  // flat_example_.idScoreLists[i] would corrupt the allocator and crash.
  // thread_local keeps allocator
  // pressure low: each worker thread reuses its own buffers across
  // calls; only initialized on first call.
  thread_local MockFeatureExample local_example;
  local_example.resize(100, 50, 0);

  // structData is mutated by the generated code even without
  // story_content seeding (e.g. `c->structData[off] += 1e-15f` in
  // archetype templates), so it also needs to be per-call. We snapshot
  // from flat_struct_data_ (the read-only template populated in
  // initializeFlatDispatch) into a thread_local buffer once per call.
  thread_local std::vector<float> local_struct;
  if (local_struct.size() != static_cast<size_t>(flat_struct_size_)) {
    local_struct.assign(flat_struct_data_.get(),
                        flat_struct_data_.get() + flat_struct_size_);
  } else {
    std::copy(flat_struct_data_.get(),
              flat_struct_data_.get() + flat_struct_size_,
              local_struct.begin());
  }
  if (story_content && story_content_length > 0) {
    int len = std::min(story_content_length, flat_struct_size_);
    for (int i = 0; i < len; ++i) {
      local_struct[i] = static_cast<float>(story_content[i]) / 255.0f;
    }
  }

  CopyContext ctx;
  ctx.tables = flat_tables_;
  ctx.structData = local_struct.data();
  ctx.structSize = static_cast<int>(local_struct.size());
  ctx.example = &local_example;
  ctx.features = flat_features_.data();
  ctx.numFeatures = static_cast<int>(flat_features_.size());
  ctx.queryKeys = input_sparse.data();
  ctx.numKeys = static_cast<int>(input_sparse.size());
  ctx.hashTables = flat_hash_tables_;
  ctx.numHashTables = 4;
  ctx.storyContent = story_content;
  ctx.storyContentLength = story_content_length;

  // Atomic cursor advance so concurrent calls don't race on
  // flat_pos_. Reserves `count` slots in one fetch_add; each call
  // therefore runs N consecutive copy functions from the shuffled
  // vector, just starting at a different offset depending on
  // interleaving. Distribution remains uniform.
  const size_t total = flat_copies_.size();
  size_t start = flat_pos_.fetch_add(static_cast<size_t>(count),
                                     std::memory_order_relaxed) %
      total;
  // Memory-streaming lever: FEEDSIM_SWEEP_N reads per call over a large
  // read-only buffer, folded into live state so it can't be elided. No-op
  // when FEEDSIM_SWEEP_N=0. Hoist the enabled check out of the loop.
  const int sweep_n = dcperf::feature_extractors::helpers::sweepReadsPerCall();
  for (int i = 0; i < count; ++i) {
    flat_copies_[(start + static_cast<size_t>(i)) % total](&ctx);
    if (sweep_n > 0) {
      local_struct[0] += dcperf::feature_extractors::helpers::runStrideSweep(
          start + static_cast<size_t>(i));
    }
  }
}

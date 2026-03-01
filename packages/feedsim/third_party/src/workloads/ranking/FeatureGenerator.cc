/**
 * FeatureGenerator.cc - Shared Feature Generator Implementation
 *
 * Phase 7: Client-Side Feature Generation with FBThrift
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 */

#include "FeatureGenerator.h"

#include <chrono>
#include <cmath>

namespace ranking {

FeatureGenerator::FeatureGenerator(
    const FeatureGeneratorConfig& config,
    int thread_id)
    : config_(config), thread_id_(thread_id) {
  // Calculate actual seed based on configuration
  if (config_.seed == static_cast<unsigned>(-1)) {
    // Time-based random seed for non-deterministic behavior
    actual_seed_ = std::chrono::system_clock::now().time_since_epoch().count() +
        thread_id_;
  } else {
    // Deterministic seed (default 42 or user-specified)
    actual_seed_ = config_.seed + thread_id_;
  }
  rng_.seed(actual_seed_);
}

std::vector<float> FeatureGenerator::generateDenseFeatures(int batch_size) {
  int total_size = batch_size * config_.num_dense_features;
  std::vector<float> features(total_size);

  // Generate random dense features using log-normal distribution
  // to mimic real-world Criteo dataset characteristics
  for (int i = 0; i < total_size; ++i) {
    float normal_val = dense_dist_(rng_);
    features[i] = std::exp(1.5f + normal_val);
  }

  return features;
}

std::vector<int64_t> FeatureGenerator::generateSparseFeatures(int batch_size) {
  int total_size = batch_size * config_.num_sparse_features;
  std::vector<int64_t> features(total_size);

  // Generate random sparse feature indices
  // Each feature index is bounded by its corresponding embedding table size
  for (int i = 0; i < total_size; ++i) {
    int feature_idx = i % config_.num_sparse_features;
    int64_t max_val = config_.embedding_table_sizes[feature_idx];
    features[i] = rng_() % max_val;
  }

  return features;
}

void FeatureGenerator::resetSeed() {
  rng_.seed(actual_seed_);
}

} // namespace ranking

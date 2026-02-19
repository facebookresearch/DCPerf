/**
 * FeatureGenerator.h - Shared Feature Generator for FeedSim DLRM
 *
 * This class provides feature generation capabilities that can be used on both
 * client (DriverNodeRank) and server (LeafNodeRank) sides for DLRM inference.
 *
 * Phase 7: Client-Side Feature Generation with FBThrift
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 */

#pragma once

#include <cstdint>
#include <random>
#include <vector>

namespace ranking {

/**
 * Configuration for feature generation.
 */
struct FeatureGeneratorConfig {
  int batch_size = 256;
  int num_dense_features = 13;
  int num_sparse_features = 26;
  unsigned seed = 42;

  // Embedding table max sizes (for sparse feature bounds)
  std::vector<int64_t> embedding_table_sizes;

  FeatureGeneratorConfig() {
    // Default embedding table sizes from trained DLRM model
    embedding_table_sizes = {
        50000, 39060, 17295, 7424,  20265, 3,     7122,  1543, 63,
        50000, 50000, 50000, 10,    2209,  11938, 155,   4,    976,
        14,    50000, 50000, 50000, 50000, 12973, 108,   36,
    };
  }
};

/**
 * FeatureGenerator - Generates DLRM features for client or server use.
 *
 * Thread Safety:
 *   - Each thread should have its own FeatureGenerator instance
 *   - Not thread-safe for concurrent access
 */
class FeatureGenerator {
 public:
  /**
   * Construct a FeatureGenerator with the given configuration.
   *
   * @param config Configuration parameters
   * @param thread_id Unique thread identifier for seed differentiation
   */
  explicit FeatureGenerator(
      const FeatureGeneratorConfig& config,
      int thread_id = 0);

  /**
   * Generate dense features.
   *
   * Dense features are continuous values following a log-normal distribution
   * to mimic real-world Criteo dataset characteristics.
   *
   * @param batch_size Number of samples to generate
   * @return Vector of dense features (batch_size * num_dense_features)
   */
  std::vector<float> generateDenseFeatures(int batch_size);

  /**
   * Generate sparse features.
   *
   * Sparse features are integer indices into embedding tables, uniformly
   * distributed within the bounds of each table.
   *
   * @param batch_size Number of samples to generate
   * @return Vector of sparse feature indices (batch_size * num_sparse_features)
   */
  std::vector<int64_t> generateSparseFeatures(int batch_size);

  /**
   * Reset RNG to initial seed state.
   * Useful for reproducibility testing.
   */
  void resetSeed();

  /**
   * Get configuration.
   */
  const FeatureGeneratorConfig& getConfig() const {
    return config_;
  }

 private:
  FeatureGeneratorConfig config_;
  int thread_id_;
  unsigned actual_seed_;
  std::mt19937 rng_;
  std::normal_distribution<float> dense_dist_{0.0f, 1.0f};
};

} // namespace ranking

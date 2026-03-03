/**
 * dlrm.h - DLRM Inference for FeedSim
 *
 * This header provides the DLRM inference class for use in FeedSim's
 * LeafNodeRank as a drop-in replacement for PageRank.
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 */

#pragma once

#ifdef FEEDSIM_USE_DLRM

// Forward declare torch types to avoid including LibTorch headers here.
// This prevents conflicts between LibTorch's c10 library and oldisim's Log.h.
namespace torch {
namespace jit {
class Module;
} // namespace jit
} // namespace torch

#include <memory>
#include <random>
#include <string>
#include <vector>

namespace ranking {
namespace dwarfs {

/**
 * Configuration for DLRM inference.
 */
struct DLRMParams {
  // Model file path (TorchScript .pt file)
  std::string model_path;

  // Feature dimensions
  int num_dense_features = 13;
  int num_sparse_features = 26;

  // Inference settings
  int batch_size = 256;
  int num_threads = 8;

  // Embedding table max sizes (for feature generation bounds)
  std::vector<int64_t> embedding_table_sizes;

  DLRMParams() {
    // Default embedding table sizes from trained model
    embedding_table_sizes = {
        50000, 39060, 17295, 7424,  20265, 3,     7122,  1543, 63,
        50000, 50000, 50000, 10,    2209,  11938, 155,   4,    976,
        14,    50000, 50000, 50000, 50000, 12973, 108,   36,
    };
  }
};

/**
 * DLRM Inference Engine.
 *
 * Provides a similar interface to PageRank for easy integration into
 * the existing FeedSim request handling flow.
 *
 * Thread Safety:
 *   - The model itself is thread-safe for concurrent inference
 *   - Each thread should use its own thread_id for per-thread state
 */
class DLRM {
 public:
  /**
   * Construct a DLRM inference engine.
   *
   * @param params Configuration parameters
   * @param num_thread_instances Number of thread instances to pre-allocate
   * @param seed Random seed for feature generation (0 = use system clock)
   */
  explicit DLRM(
      const DLRMParams& params,
      int num_thread_instances = 1,
      unsigned seed = 0);

  ~DLRM();

  // Disable copy
  DLRM(const DLRM&) = delete;
  DLRM& operator=(const DLRM&) = delete;

  /**
   * Run DLRM inference with synthetic feature generation.
   *
   * This method signature mirrors PageRank::rank() for easy integration.
   *
   * @param num_inferences Number of inference calls to make
   * @param batch_size Batch size per inference call
   * @return Total number of predictions made
   */
  int infer(int num_inferences, int batch_size);

  /**
   * Run DLRM inference with client-provided features (Phase 7).
   *
   * This method accepts pre-generated features from the client instead of
   * generating them server-side. Used for client-side feature generation.
   *
   * @param dense_features Pointer to dense feature array (batch_size * num_dense_features)
   * @param sparse_features Pointer to sparse feature array (batch_size * num_sparse_features)
   * @param batch_size Batch size
   * @param num_inferences Number of inference calls to make
   * @return Total number of predictions made
   */
  int inferWithFeatures(
      const float* dense_features,
      const int64_t* sparse_features,
      int batch_size,
      int num_inferences = 1);

  /**
   * Warmup the model for optimal inference performance.
   *
   * @param num_iterations Number of warmup iterations
   */
  void warmup(int num_iterations = 10);

  /**
   * Check if model is loaded.
   */
  bool isModelLoaded() const {
    return model_loaded_;
  }

 private:
  // Use pimpl idiom to hide LibTorch implementation details
  struct Impl;
  std::unique_ptr<Impl> pimpl_;

  // Configuration
  DLRMParams params_;
  bool model_loaded_ = false;
};

} // namespace dwarfs
} // namespace ranking

#endif // FEEDSIM_USE_DLRM

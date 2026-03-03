/**
 * dlrm.cpp - DLRM Inference Implementation for FeedSim
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 */

#ifdef FEEDSIM_USE_DLRM

#include "dlrm.h"

// Include LibTorch headers only in the implementation file
// to avoid conflicts with oldisim's Log.h
#include <torch/script.h>
#include <torch/torch.h>

#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <folly/ScopeGuard.h>

namespace ranking {
namespace dwarfs {

// Implementation class that holds LibTorch-specific data
struct DLRM::Impl {
  torch::jit::script::Module model;

  // Per-thread state for synthetic feature generation
  struct ThreadState {
    std::mt19937 rng;
    std::normal_distribution<float> dense_dist{0.0f, 1.0f};
    std::vector<float> dense_buffer;
    std::vector<int64_t> sparse_buffer;
  };
  std::vector<std::unique_ptr<ThreadState>> thread_states;

  alignas(64) std::mutex thread_id_lifo_mutex;
  std::vector<int> thread_id_lifo;

  void loadModel(const std::string& model_path) {
    model = torch::jit::load(model_path);
    model.eval();
    model = torch::jit::optimize_for_inference(model);
  }

  void initializeThreadState(
      int num_threads,
      unsigned seed,
      int batch_size,
      int num_dense_features,
      int num_sparse_features) {
    thread_states.resize(num_threads);

    std::lock_guard<std::mutex> lock(thread_id_lifo_mutex);
    for (int i = 0; i < num_threads; ++i) {
      auto state = std::make_unique<ThreadState>();

      // Initialize RNG
      // seed == static_cast<unsigned>(-1) means use time-based random seed
      // Any other seed value (including default 42) is used directly
      unsigned actual_seed;
      if (seed == static_cast<unsigned>(-1)) {
        // Time-based random seed for non-deterministic behavior
        actual_seed =
            std::chrono::system_clock::now().time_since_epoch().count() + i;
      } else {
        // Deterministic seed (default 42 or user-specified)
        actual_seed = seed + i; // Unique seed per thread
      }
      state->rng.seed(actual_seed);

      // Pre-allocate buffers
      state->dense_buffer.resize(batch_size * num_dense_features);
      state->sparse_buffer.resize(batch_size * num_sparse_features);

      thread_states[i] = std::move(state);
      thread_id_lifo.push_back(i);
    }
  }

  at::Tensor generateDenseFeatures(
      int thread_id,
      int batch_size,
      int num_dense_features) {
    auto& state = thread_states[thread_id];

    // Resize buffer if needed
    size_t required_size = batch_size * num_dense_features;
    if (state->dense_buffer.size() < required_size) {
      state->dense_buffer.resize(required_size);
    }

    // Generate random dense features (log-normal distribution to mimic Criteo)
    for (size_t i = 0; i < required_size; ++i) {
      float normal_val = state->dense_dist(state->rng);
      state->dense_buffer[i] = std::exp(1.5f + normal_val);
    }

    return torch::from_blob(
        state->dense_buffer.data(),
        {static_cast<int64_t>(batch_size), num_dense_features},
        torch::kFloat32);
  }

  at::Tensor generateSparseFeatures(
      int thread_id,
      int batch_size,
      int num_sparse_features,
      const std::vector<int64_t>& embedding_table_sizes) {
    auto& state = thread_states[thread_id];

    // Resize buffer if needed
    size_t required_size = batch_size * num_sparse_features;
    if (state->sparse_buffer.size() < required_size) {
      state->sparse_buffer.resize(required_size);
    }

    // Generate random sparse feature indices
    for (size_t i = 0; i < required_size; ++i) {
      int feature_idx = i % num_sparse_features;
      int64_t max_val = embedding_table_sizes[feature_idx];
      state->sparse_buffer[i] = state->rng() % max_val;
    }

    return torch::from_blob(
        state->sparse_buffer.data(),
        {static_cast<int64_t>(batch_size), num_sparse_features},
        torch::kInt64);
  }

  int get_avail_thread_id() {
    std::lock_guard<std::mutex> lock(thread_id_lifo_mutex);
    if (thread_id_lifo.empty()) {
      throw std::runtime_error("More parallelism than allocated threads in DLRM");
    }
    int thread_id = thread_id_lifo.back();
    thread_id_lifo.pop_back();
    return thread_id;
  }

  void put_avail_thread_id(int thread_id) {
    std::lock_guard<std::mutex> lock(thread_id_lifo_mutex);
    thread_id_lifo.push_back(thread_id);
  }
};

DLRM::DLRM(const DLRMParams& params, int num_thread_instances, unsigned seed)
    : pimpl_(std::make_unique<Impl>()), params_(params) {
  // Set number of inference threads
  at::set_num_threads(params_.num_threads);

  // Enable JIT optimizations
  torch::jit::setGraphExecutorOptimize(true);

  // Load model if path provided
  if (!params_.model_path.empty()) {
    try {
      std::cout << "Loading DLRM model from: " << params_.model_path
                << std::endl;
      pimpl_->loadModel(params_.model_path);
      model_loaded_ = true;
      std::cout << "DLRM model loaded successfully." << std::endl;
    } catch (const c10::Error& e) {
      throw std::runtime_error(
          "Failed to load DLRM model: " + std::string(e.what()));
    }
  }

  // Initialize per-thread state
  pimpl_->initializeThreadState(
      num_thread_instances,
      seed,
      params_.batch_size,
      params_.num_dense_features,
      params_.num_sparse_features);

  // Warmup
  if (model_loaded_) {
    warmup(10);
  }
}

DLRM::~DLRM() = default;

int DLRM::infer(int num_inferences, int batch_size) {
  if (!model_loaded_) {
    throw std::runtime_error("Model not loaded");
  }

  int thread_id = pimpl_->get_avail_thread_id();
  SCOPE_EXIT { pimpl_->put_avail_thread_id(thread_id); };

  if (thread_id < 0 ||
      thread_id >= static_cast<int>(pimpl_->thread_states.size())) {
    throw std::out_of_range("Invalid thread_id: " + std::to_string(thread_id));
  }

  int total_predictions = 0;

  for (int i = 0; i < num_inferences; ++i) {
    // Generate features
    auto dense_tensor = pimpl_->generateDenseFeatures(
        thread_id, batch_size, params_.num_dense_features);
    auto sparse_tensor = pimpl_->generateSparseFeatures(
        thread_id,
        batch_size,
        params_.num_sparse_features,
        params_.embedding_table_sizes);

    // Run inference
    std::vector<torch::jit::IValue> inputs;
    inputs.push_back(dense_tensor);
    inputs.push_back(sparse_tensor);

    torch::NoGradGuard no_grad;
    auto output = pimpl_->model.forward(inputs).toTensor();

    // Count predictions (simulating actual work with the output)
    total_predictions += output.numel();
  }

  return total_predictions;
}

int DLRM::inferWithFeatures(
    const float* dense_features,
    const int64_t* sparse_features,
    int batch_size,
    int num_inferences) {
  if (!model_loaded_) {
    throw std::runtime_error("Model not loaded");
  }

  int total_predictions = 0;

  for (int i = 0; i < num_inferences; ++i) {
    // Create tensors from client-provided features
    // Note: from_blob does not copy data, so the original arrays must remain valid
    auto dense_tensor = torch::from_blob(
        const_cast<float*>(dense_features),
        {static_cast<int64_t>(batch_size), params_.num_dense_features},
        torch::kFloat32);

    auto sparse_tensor = torch::from_blob(
        const_cast<int64_t*>(sparse_features),
        {static_cast<int64_t>(batch_size), params_.num_sparse_features},
        torch::kInt64);

    // Run inference
    std::vector<torch::jit::IValue> inputs;
    inputs.push_back(dense_tensor);
    inputs.push_back(sparse_tensor);

    torch::NoGradGuard no_grad;
    auto output = pimpl_->model.forward(inputs).toTensor();

    // Count predictions (simulating actual work with the output)
    total_predictions += output.numel();
  }

  return total_predictions;
}

void DLRM::warmup(int num_iterations) {
  if (!model_loaded_) {
    std::cerr << "Warning: Cannot warmup - model not loaded" << std::endl;
    return;
  }

  std::cout << "Warming up DLRM model (" << num_iterations << " iterations)..."
            << std::endl;

  // Use thread 0 for warmup
  for (int i = 0; i < num_iterations; ++i) {
    infer(1, params_.batch_size);
  }

  std::cout << "DLRM warmup complete." << std::endl;
}

} // namespace dwarfs
} // namespace ranking

#endif // FEEDSIM_USE_DLRM

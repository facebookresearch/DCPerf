/**
 * dlrm.cpp - DLRM Inference Implementation for FeedSim
 *
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 */

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

  // Per-thread state for synthetic feature generation + an isolated
  // JIT Module clone. torch::jit::Module::forward() is NOT thread-safe
  // when called concurrently on the same Module instance — the JIT
  // interpreter mutates internal state (e.g. interpreter stack,
  // intermediate tensors) and concurrent invocations race on the
  // backing allocator, producing SIGSEGV in je_large_dalloc. Each
  // worker thread owns a deep-cloned Module via model_copy, so
  // concurrent forward() calls touch disjoint state. See t16
  // SIGSEGV crash (2026-05-18) for the original failure.
  //
  // forward_mutex (t29, 2026-05-28): the LIFO checkout in
  // get_avail_thread_id() / put_avail_thread_id() is supposed to
  // prevent two callers from holding the same thread_id at once, but
  // the leaf still SIGSEGVs inside torch::jit::InterpreterStateImpl::
  // runTemplate at over-saturation (t27 q=160+/inst on BGM/Grace,
  // q=50+ on CPL). Likely cause: torch::jit::Module::clone() does NOT
  // fully isolate all interpreter state. Per-clone mutex is a defensive
  // safety net — uncontended when the LIFO is doing its job, prevents
  // the segfault if it's not.
  struct ThreadState {
    std::mt19937 rng;
    std::normal_distribution<float> dense_dist{0.0f, 1.0f};
    std::vector<float> dense_buffer;
    std::vector<int64_t> sparse_buffer;
    std::unique_ptr<torch::jit::script::Module> model_copy;
    std::mutex forward_mutex;
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
      int num_sparse_features,
      bool clone_model) {
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

      // Deep-clone the JIT Module so this thread's forward() touches
      // disjoint interpreter state. clone() copies submodules + tensors;
      // copy() would share them and re-introduce the race.
      // Skipped when the parent DLRM has no model loaded (test paths).
      if (clone_model) {
        state->model_copy = std::make_unique<torch::jit::script::Module>(
            model.clone());
      }

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

  // Cap inter-op thread pool to 1. Without this, PyTorch's inter-op pool
  // defaults to available_concurrency() (= nproc on a leaf process). At
  // model warmup + on first forward() from each ThriftSrv.IO worker,
  // libtorch lazily spawns nproc inter-op threads — which adds up to
  // nproc^2 GlobalCPUThread-named threads (= 30,976 on BGM with 176
  // logical cores), all stuck in __futex_wait, eventually deadlocking
  // the kernel scheduler. See progress log 2026-05-15 (t12/t13).
  //
  // libtorch treats set_num_interop_threads as call-once-per-process:
  // subsequent invocations throw. Guard so a second DLRM construction
  // (unit tests, re-init) doesn't crash.
  static std::once_flag interop_threads_once;
  std::call_once(interop_threads_once, [] { at::set_num_interop_threads(1); });

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

  // Initialize per-thread state. Deep-clones the JIT Module per thread
  // when model is loaded, to avoid concurrent forward() race on shared
  // interpreter state (see ThreadState comment + t16 SIGSEGV).
  pimpl_->initializeThreadState(
      num_thread_instances,
      seed,
      params_.batch_size,
      params_.num_dense_features,
      params_.num_sparse_features,
      model_loaded_);

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
  // Validate before arming SCOPE_EXIT so a bogus id doesn't get pushed back
  // into thread_id_lifo (would silently corrupt the free-list).
  if (thread_id < 0 ||
      thread_id >= static_cast<int>(pimpl_->thread_states.size())) {
    throw std::out_of_range("Invalid thread_id: " + std::to_string(thread_id));
  }
  SCOPE_EXIT { pimpl_->put_avail_thread_id(thread_id); };

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

    // Run inference on the per-thread Module clone (not the shared
    // pimpl_->model) to avoid concurrent forward() race. forward_mutex
    // serializes calls per clone as a safety net on top of the LIFO
    // checkout — see ThreadState comment.
    std::vector<torch::jit::IValue> inputs;
    inputs.push_back(dense_tensor);
    inputs.push_back(sparse_tensor);

    torch::NoGradGuard no_grad;
    auto& ts = *pimpl_->thread_states[thread_id];
    std::lock_guard<std::mutex> forward_lock(ts.forward_mutex);
    auto output = ts.model_copy->forward(inputs).toTensor();

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

  // Acquire a thread_id so we can use this thread's Module clone. Same
  // race avoidance as DLRM::infer.
  int thread_id = pimpl_->get_avail_thread_id();
  // Validate before arming SCOPE_EXIT so a bogus id doesn't get pushed back
  // into thread_id_lifo (would silently corrupt the free-list).
  if (thread_id < 0 ||
      thread_id >= static_cast<int>(pimpl_->thread_states.size())) {
    throw std::out_of_range("Invalid thread_id: " + std::to_string(thread_id));
  }
  SCOPE_EXIT { pimpl_->put_avail_thread_id(thread_id); };

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

    // Run inference on this thread's Module clone. forward_mutex
    // serializes calls per clone — see ThreadState comment.
    std::vector<torch::jit::IValue> inputs;
    inputs.push_back(dense_tensor);
    inputs.push_back(sparse_tensor);

    torch::NoGradGuard no_grad;
    auto& ts = *pimpl_->thread_states[thread_id];
    std::lock_guard<std::mutex> forward_lock(ts.forward_mutex);
    auto output = ts.model_copy->forward(inputs).toTensor();

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

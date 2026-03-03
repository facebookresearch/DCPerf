// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <folly/Range.h>
#include <folly/compression/Compression.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/init/Init.h>

#include <thrift/lib/cpp2/protocol/CompactProtocol.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include "oldisim/LeafNodeServer.h"
#include "oldisim/NodeThread.h"
#include "oldisim/ParentConnection.h"
#include "oldisim/QueryContext.h"
#include "oldisim/Util.h"

#include "LeafNodeRankCmdline.h"
#include "RequestTypes.h"

#include "TimekeeperPool.h"
#include "dwarfs/pagerank.h"

#ifdef FEEDSIM_USE_DLRM
#include "dwarfs/dlrm.h"
#endif

#include "if/gen-cpp2/ranking_types.h"

#include "../search/ICacheBuster.h"
#include "../search/PointerChase.h"

#include "generators/RankingGenerators.h"

// Shared configuration flags
static gengetopt_args_info args;

constexpr auto kMaxResponseSize = 1u << 12u;
const auto kNumNops = 6;
const auto kNumNopIterations = 60;
const auto kNumCompressIterations = 100;
const auto kNumICacheBusterMethods = 100000;
const auto kPointerChaseSize = 10000000;
const auto kPageRankThreshold = 1e-4;

// I/O latency distribution types for Phase 3
enum class IOLatencyDistType {
  FIXED,       // Fixed latency (original behavior)
  EXPONENTIAL, // Exponential distribution (memoryless, models queue delays)
  LOGNORMAL    // Lognormal distribution (models real-world service latencies)
};

struct ThreadData {
  std::shared_ptr<folly::CPUThreadPoolExecutor> cpuThreadPool;
  std::shared_ptr<folly::CPUThreadPoolExecutor> srvCPUThreadPool;
  std::shared_ptr<folly::CPUThreadPoolExecutor> srvIOThreadPool;
  std::shared_ptr<folly::IOThreadPoolExecutor> ioThreadPool;
  std::shared_ptr<ranking::TimekeeperPool> timekeeperPool;
  std::unique_ptr<ranking::dwarfs::PageRank> page_ranker;
#ifdef FEEDSIM_USE_DLRM
  std::shared_ptr<ranking::dwarfs::DLRM> dlrm_ranker;
#endif
  std::unique_ptr<search::PointerChase> pointer_chaser;
  std::unique_ptr<ICacheBuster> icache_buster;
  std::default_random_engine rng;
  std::gamma_distribution<double> latency_distribution;
  std::string random_string;

  // Phase 3: I/O latency distribution support
  IOLatencyDistType io_latency_dist_type = IOLatencyDistType::FIXED;
  std::exponential_distribution<double> io_exponential_dist;
  std::lognormal_distribution<double> io_lognormal_dist;
  int io_latency_mean_ms = 200;
  int io_latency_min_ms = 50;   // Minimum bound to prevent too-fast responses
  int io_latency_max_ms = 1000; // Maximum bound to prevent extreme outliers (was 5000)

  // Mutex for thread-safe RNG access (RNG state is not thread-safe)
  std::mutex rng_mutex;

  // Get next I/O latency based on distribution type
  // IMPORTANT: This function MUST be called from the handler thread (before async)
  // to avoid race conditions on the RNG state.
  int getNextIOLatencyMs() {
    std::lock_guard<std::mutex> lock(rng_mutex);
    switch (io_latency_dist_type) {
      case IOLatencyDistType::FIXED:
        return io_latency_mean_ms;
      case IOLatencyDistType::EXPONENTIAL:
        // Exponential distribution with specified mean, bounded
        return std::max(io_latency_min_ms,
            std::min(io_latency_max_ms, static_cast<int>(io_exponential_dist(rng))));
      case IOLatencyDistType::LOGNORMAL:
        // Lognormal distribution with tighter bounds to reduce tail latency variance
        return std::max(io_latency_min_ms,
            std::min(io_latency_max_ms, static_cast<int>(io_lognormal_dist(rng))));
      default:
        return io_latency_mean_ms;
    }
  }
};

// Enum for workload type
enum class WorkloadType {
  PAGERANK,
  DLRM
};

// Global workload type
static WorkloadType g_workload_type = WorkloadType::PAGERANK;

// Global graph that will be shared across threads
CSRGraph<int32_t> g_shared_graph;

#ifdef FEEDSIM_USE_DLRM
void ThreadStartup(
    oldisim::NodeThread& thread,
    std::vector<ThreadData>& thread_data,
    ranking::dwarfs::PageRankParams& params,
    const std::shared_ptr<folly::CPUThreadPoolExecutor>& cpuThreadPool,
    const std::shared_ptr<folly::CPUThreadPoolExecutor>& srvCPUThreadPool,
    const std::shared_ptr<folly::CPUThreadPoolExecutor>& srvIOThreadPool,
    const std::shared_ptr<folly::IOThreadPoolExecutor>& ioThreadPool,
    const std::shared_ptr<ranking::TimekeeperPool>& timekeeperPool,
    const std::shared_ptr<ranking::dwarfs::DLRM>& shared_dlrm_ranker) {
  auto& this_thread = thread_data[thread.get_thread_num()];
  this_thread.cpuThreadPool = cpuThreadPool;
  this_thread.srvCPUThreadPool = srvCPUThreadPool;
  this_thread.srvIOThreadPool = srvIOThreadPool;
  this_thread.ioThreadPool = ioThreadPool;
  this_thread.timekeeperPool = timekeeperPool;

  // Store shared DLRM ranker
  this_thread.dlrm_ranker = shared_dlrm_ranker;

  unsigned noderank_seed;
  if (args.node_rank_seed_given) {
    noderank_seed = static_cast<unsigned>(args.node_rank_seed_arg);
  } else {
    noderank_seed = std::chrono::system_clock::now().time_since_epoch().count();
  }

  unsigned pointer_chase_seed;
  if (args.pointer_chase_seed_given) {
    pointer_chase_seed = static_cast<unsigned>(args.pointer_chase_seed_arg);
  } else {
    pointer_chase_seed =
        std::chrono::system_clock::now().time_since_epoch().count();
  }

  // Only initialize PageRank if we're using it
  if (g_workload_type == WorkloadType::PAGERANK) {
    unsigned page_rank_seed;
    if (args.page_rank_seed_given) {
      page_rank_seed = static_cast<unsigned>(args.page_rank_seed_arg);
    } else {
      page_rank_seed = std::chrono::system_clock::now().time_since_epoch().count();
    }
    auto graph = params.makeGraphCopy(g_shared_graph);
    this_thread.page_ranker = std::make_unique<ranking::dwarfs::PageRank>(
        std::move(graph), args.cpu_threads_arg, page_rank_seed);
    this_thread.icache_buster =
        std::make_unique<ICacheBuster>(kNumICacheBusterMethods);
  }

  this_thread.pointer_chaser = std::make_unique<search::PointerChase>(
      kPointerChaseSize, pointer_chase_seed);
  this_thread.rng.seed(noderank_seed);

  const double alpha = 0.7;
  const double beta = 20000;
  this_thread.latency_distribution =
      std::gamma_distribution<double>(alpha, beta);

  this_thread.random_string = RandomString(args.random_data_size_arg);

  // Phase 3: Initialize I/O latency distributions
  this_thread.io_latency_mean_ms = args.io_latency_mean_ms_arg;
  std::string io_dist_str = args.io_latency_distribution_arg;
  if (io_dist_str == "exponential") {
    this_thread.io_latency_dist_type = IOLatencyDistType::EXPONENTIAL;
    double rate = 1.0 / static_cast<double>(args.io_latency_mean_ms_arg);
    this_thread.io_exponential_dist = std::exponential_distribution<double>(rate);
  } else if (io_dist_str == "lognormal") {
    this_thread.io_latency_dist_type = IOLatencyDistType::LOGNORMAL;
    double mean = static_cast<double>(args.io_latency_mean_ms_arg);
    double stddev = static_cast<double>(args.io_latency_stddev_ms_arg);
    double variance = stddev * stddev;
    double mu = std::log(mean * mean / std::sqrt(variance + mean * mean));
    double sigma = std::sqrt(std::log(1.0 + variance / (mean * mean)));
    this_thread.io_lognormal_dist = std::lognormal_distribution<double>(mu, sigma);
  } else {
    this_thread.io_latency_dist_type = IOLatencyDistType::FIXED;
  }
}
#endif

void ThreadStartup(
    oldisim::NodeThread& thread,
    std::vector<ThreadData>& thread_data,
    ranking::dwarfs::PageRankParams& params,
    const std::shared_ptr<folly::CPUThreadPoolExecutor>& cpuThreadPool,
    const std::shared_ptr<folly::CPUThreadPoolExecutor>& srvCPUThreadPool,
    const std::shared_ptr<folly::CPUThreadPoolExecutor>& srvIOThreadPool,
    const std::shared_ptr<folly::IOThreadPoolExecutor>& ioThreadPool,
    const std::shared_ptr<ranking::TimekeeperPool>& timekeeperPool) {
  auto& this_thread = thread_data[thread.get_thread_num()];
  auto graph = params.makeGraphCopy(g_shared_graph);
  this_thread.cpuThreadPool = cpuThreadPool;
  this_thread.srvCPUThreadPool = srvCPUThreadPool;
  this_thread.srvIOThreadPool = srvIOThreadPool;
  this_thread.ioThreadPool = ioThreadPool;
  this_thread.timekeeperPool = timekeeperPool;
  unsigned noderank_seed;
  if (args.node_rank_seed_given) {
    noderank_seed = static_cast<unsigned>(args.node_rank_seed_arg);
  } else {
    noderank_seed = std::chrono::system_clock::now().time_since_epoch().count();
  }

  unsigned page_rank_seed;
  if (args.page_rank_seed_given) {
    page_rank_seed = static_cast<unsigned>(args.page_rank_seed_arg);
  } else {
    page_rank_seed = std::chrono::system_clock::now().time_since_epoch().count();
  }

  unsigned pointer_chase_seed;
  if (args.pointer_chase_seed_given) {
    pointer_chase_seed = static_cast<unsigned>(args.pointer_chase_seed_arg);
  } else {
    pointer_chase_seed =
        std::chrono::system_clock::now().time_since_epoch().count();
  }

  this_thread.page_ranker = std::make_unique<ranking::dwarfs::PageRank>(
      std::move(graph), args.cpu_threads_arg, page_rank_seed);
  this_thread.icache_buster =
      std::make_unique<ICacheBuster>(kNumICacheBusterMethods);
  this_thread.pointer_chaser = std::make_unique<search::PointerChase>(
      kPointerChaseSize, pointer_chase_seed);
  this_thread.rng.seed(noderank_seed);

  const double alpha = 0.7;
  const double beta = 20000;
  this_thread.latency_distribution =
      std::gamma_distribution<double>(alpha, beta);

  this_thread.random_string = RandomString(args.random_data_size_arg);

  // Phase 3: Initialize I/O latency distributions
  this_thread.io_latency_mean_ms = args.io_latency_mean_ms_arg;
  std::string io_dist_str = args.io_latency_distribution_arg;
  if (io_dist_str == "exponential") {
    this_thread.io_latency_dist_type = IOLatencyDistType::EXPONENTIAL;
    // Exponential distribution with rate lambda = 1/mean
    double rate = 1.0 / static_cast<double>(args.io_latency_mean_ms_arg);
    this_thread.io_exponential_dist = std::exponential_distribution<double>(rate);
  } else if (io_dist_str == "lognormal") {
    this_thread.io_latency_dist_type = IOLatencyDistType::LOGNORMAL;
    // Convert mean and stddev to lognormal parameters (mu, sigma)
    double mean = static_cast<double>(args.io_latency_mean_ms_arg);
    double stddev = static_cast<double>(args.io_latency_stddev_ms_arg);
    double variance = stddev * stddev;
    double mu = std::log(mean * mean / std::sqrt(variance + mean * mean));
    double sigma = std::sqrt(std::log(1.0 + variance / (mean * mean)));
    this_thread.io_lognormal_dist = std::lognormal_distribution<double>(mu, sigma);
  } else {
    this_thread.io_latency_dist_type = IOLatencyDistType::FIXED;
  }
}

std::string compressPayload(const std::string& data, int result) {
  folly::StringPiece output(
      data.data(),
      std::min(args.compression_data_size_arg, args.random_data_size_arg));
  auto codec =
      folly::compression::getCodec(folly::compression::CodecType::ZSTD);
  std::string compressed = codec->compress(output);
  return std::move(compressed);
}

std::string decompressPayload(const std::string& data) {
  auto codec =
      folly::compression::getCodec(folly::compression::CodecType::ZSTD);
  std::string decompressed = codec->uncompress(data);
  return decompressed;
}

std::unique_ptr<folly::IOBuf> compressThrift(
    std::unique_ptr<folly::IOBuf> buf) {
  auto codec =
      folly::compression::getCodec(folly::compression::CodecType::ZSTD);
  auto compressed_buf = codec->compress(buf.get());
  return compressed_buf;
}

folly::IOBufQueue serializePayload(const ranking::RankingResponse& resp) {
  folly::IOBufQueue bufq;
  apache::thrift::CompactSerializer::serialize(resp, &bufq);
  return std::move(bufq);
}

ranking::RankingResponse deserializePayload(const folly::IOBuf* buf) {
  ranking::RankingResponse resp;
  apache::thrift::CompactSerializer::deserialize(buf, resp);
  return resp;
}

#ifdef FEEDSIM_USE_DLRM
static int dlrmInferenceServerSideDataGeneration(ThreadData& this_thread,
                                                 int total_num_inferences) {
  int num_inferences_max =
      (total_num_inferences + args.cpu_threads_arg - 1)
      / args.cpu_threads_arg;
  int batch_size = args.dlrm_batch_size_arg;

  std::vector<folly::Future<int>> futures;
  for (int i = 0; i < args.cpu_threads_arg; i++) {
    int num_inferences = std::min(num_inferences_max, total_num_inferences);
    auto f = folly::via(
        this_thread.cpuThreadPool.get(),
        [num_inferences, batch_size, &this_thread]() {
          return this_thread.dlrm_ranker->infer(num_inferences, batch_size);
        });
    futures.push_back(std::move(f));
    total_num_inferences -= num_inferences;
    if (total_num_inferences <= 0) break;
  }
  auto fs = folly::collectAll(std::move(futures)).get();
  int result = 0;
  for (auto& f : fs) {
    result += f.value();
  }
  return result;
}
#endif

/**
 * Phase 3: Async (non-blocking) request handler using continuation-passing style.
 *
 * This handler implements the same logic as PageRankRequestHandler but without
 * blocking .get() calls. This eliminates thread starvation on high-core CPUs
 * by returning immediately and processing the response asynchronously.
 *
 * Key differences from the blocking handler:
 * 1. No blocking .get() calls on the I/O future
 * 2. Response is sent in the final continuation
 * 3. All I/O stages are chained via .thenValue()/.thenVia()
 * 4. Uses configurable I/O latency distributions
 *
 * CRITICAL: The QueryContext is moved into a shared_ptr to extend its lifetime
 * beyond the handler return. The oldisim framework destroys the context after
 * the handler returns, but we need it to survive until the async callback.
 */
void AsyncPageRankRequestHandler(
    oldisim::NodeThread& thread,
    oldisim::QueryContext& context,
    std::vector<ThreadData>& thread_data) {
  auto& this_thread = thread_data[thread.get_thread_num()];
  int thread_id = thread.get_thread_num();

  // CRITICAL: Move the QueryContext into a shared_ptr to extend its lifetime.
  // The oldisim framework destroys the stack-allocated context after this
  // handler returns, but we need it alive until the async callback completes.
  auto context_ptr = std::make_shared<oldisim::QueryContext>(std::move(context));

  // Stage 1: ICacheBuster (synchronous, lightweight) - only for PageRank
  if (g_workload_type == WorkloadType::PAGERANK) {
    const int min_iterations = std::max(args.min_icache_iterations_arg, 0);
    const int num_iterations =
        static_cast<int>(this_thread.latency_distribution(this_thread.rng)) +
        min_iterations;
    ICacheBuster& buster = *this_thread.icache_buster;

    for (int i = 0; i < num_iterations; i++) {
      buster.RunNextMethod();
    }
  }

  // Stage 2: Ranking workload (CPU-intensive, parallelized)
  // This stage blocks briefly for CPU work, which is acceptable
  int ranking_result = 0;
  if (g_workload_type == WorkloadType::PAGERANK) {
    auto per_thread_subset = args.graph_subset_arg / args.cpu_threads_arg;

    std::vector<folly::Future<int>> futures;
    for (int i = 0; i < args.cpu_threads_arg; i++) {
      auto f = folly::via(
          this_thread.cpuThreadPool.get(),
          [i, &this_thread, per_thread_subset]() {
            return this_thread.page_ranker->rank(
                i,
                args.graph_max_iters_arg,
                kPageRankThreshold,
                args.rank_trials_per_thread_arg,
                per_thread_subset);
          });
      futures.push_back(std::move(f));
    }
    auto fs = folly::collectAll(std::move(futures)).get();
    for (auto& f : fs) {
      ranking_result += f.value();
    }
  }
#ifdef FEEDSIM_USE_DLRM
  else if (g_workload_type == WorkloadType::DLRM) {
    ranking_result = dlrmInferenceServerSideDataGeneration(
        this_thread, args.dlrm_inferences_per_request_arg);
  }
#endif

  // Capture data needed for async stages by value
  auto random_string = this_thread.random_string;
  auto srvCPUThreadPool = this_thread.srvCPUThreadPool;
  auto srvIOThreadPool = this_thread.srvIOThreadPool;
  auto ioThreadPool = this_thread.ioThreadPool;
  auto timekeeperPool = this_thread.timekeeperPool;
  search::PointerChase* pointer_chaser = this_thread.pointer_chaser.get();

  // Get I/O latency for this request (configurable distribution)
  int io_latency_ms = this_thread.getNextIOLatencyMs();

  // For multi-stage I/O simulation
  int num_io_stages = args.io_stages_arg;
  int io_stage_latency_ms = args.io_stage_latency_ms_arg;

  // Capture values for lambda captures
  int srv_io_threads = args.srv_io_threads_arg;
  int srv_threads = args.srv_threads_arg;
  int num_objects = args.num_objects_arg;
  int chase_iterations = args.chase_iterations_arg;

  // Stage 3: Async I/O simulation (NON-BLOCKING)
  // This is the critical change - we use continuation-passing style
  auto timekeeper = timekeeperPool->getTimekeeper();

  // Calculate total I/O latency
  int total_io_latency_ms = (num_io_stages > 1)
      ? (num_io_stages * io_stage_latency_ms)
      : io_latency_ms;

  // Start async chain with I/O sleep
  folly::futures::sleep(
      std::chrono::milliseconds(total_io_latency_ms), timekeeper.get())
      .via(ioThreadPool.get())
      .thenValue([ranking_result, random_string, srvIOThreadPool,
                  srv_io_threads, num_objects](folly::Unit) {
        // Stage 4: Compression and serialization
        auto compressed = compressPayload(random_string, ranking_result);
        auto per_thread_num_objects = num_objects / srv_io_threads;

        std::vector<folly::Future<int>> compressionFutures;
        for (int i = 0; i < srv_io_threads; i++) {
          auto f = folly::via(srvIOThreadPool.get(), [per_thread_num_objects]() {
            auto resp = ranking::generators::generateRandomRankingResponse(
                per_thread_num_objects);
            auto payloadiobufq = serializePayload(resp);
            auto buf = payloadiobufq.move();
            const auto compress_length = buf->computeChainDataLength() / 2;
            size_t total_size = 0;
            for (auto range : *buf) {
              if (total_size >= compress_length) break;
              auto iobuf = folly::IOBuf::copyBuffer(range.data(), range.size());
              auto c = compressThrift(std::move(iobuf));
              total_size += range.size();
            }
            return 1;
          });
          compressionFutures.push_back(std::move(f));
        }
        return folly::collectAll(std::move(compressionFutures))
            .via(srvIOThreadPool.get())
            .thenValue([ranking_result](std::vector<folly::Try<int>> results) {
              int total = ranking_result;
              for (auto& r : results) {
                if (r.hasValue()) total += r.value();
              }
              return total;
            });
      })
      .thenValue([pointer_chaser, srvCPUThreadPool, srv_threads,
                  chase_iterations](int prev_result) {
        // Stage 5: Pointer chase
        auto per_thread_chase_iterations = chase_iterations / srv_threads;

        std::vector<folly::Future<int>> chaseFutures;
        for (int i = 0; i < srv_threads; i++) {
          auto f = folly::via(srvCPUThreadPool.get(),
              [pointer_chaser, per_thread_chase_iterations]() {
                pointer_chaser->Chase(per_thread_chase_iterations);
                return 1;
              });
          chaseFutures.push_back(std::move(f));
        }
        return folly::collectAll(std::move(chaseFutures))
            .via(srvCPUThreadPool.get())
            .thenValue([prev_result](std::vector<folly::Try<int>> results) {
              int total = prev_result;
              for (auto& r : results) {
                if (r.hasValue()) total += r.value();
              }
              return total;
            });
      })
      .thenValue([context_ptr, srv_io_threads, num_objects](int final_result) {
        // Stage 6: Generate and send response
        auto per_thread_num_objects = num_objects / srv_io_threads;
        auto r = ranking::generators::generateRandomRankingResponse(
            per_thread_num_objects);
        ranking::RankingResponse resp = r;

        auto payloadiobufq = serializePayload(resp);
        auto buf = payloadiobufq.move();

        context_ptr->SendResponse(buf->data(), buf->length());
      })
      .thenError(folly::tag_t<std::exception>{}, [context_ptr](const std::exception& e) {
        // Error handling
        std::cerr << "Async request handler error: " << e.what() << std::endl;
        context_ptr->SendResponse(nullptr, 0);
      });

  // NO .get() here! Handler returns immediately, work continues asynchronously
}

#ifdef FEEDSIM_USE_DLRM
/**
 * Phase 7: DLRM Request Handler with client-side features.
 *
 * This handler processes RankingRequest messages that contain pre-generated
 * DLRM features from the client. It deserializes the features and runs
 * inference using DLRM::inferWithFeatures().
 */
void DLRMRequestHandler(
    oldisim::NodeThread& thread,
    oldisim::QueryContext& context,
    std::vector<ThreadData>& thread_data) {
  auto& this_thread = thread_data[thread.get_thread_num()];
  int thread_id = thread.get_thread_num();

  // Deserialize RankingRequest from payload
  ranking::RankingRequest request;
  try {
    folly::IOBuf buf(
        folly::IOBuf::WRAP_BUFFER,
        context.payload,
        context.payload_length);
    apache::thrift::CompactSerializer::deserialize(&buf, request);
  } catch (const std::exception& e) {
    std::cerr << "Failed to deserialize RankingRequest: " << e.what() << std::endl;
    context.SendResponse(nullptr, 0);
    return;
  }

  int result = 0;

  // Check if client provided features
  if (request.dlrm_features().has_value()) {
    const auto& features = request.dlrm_features().value();
    int batch_size = *features.batch_size();
    int total_num_inferences = *request.num_inferences();

    // Convert from Thrift types to arrays
    const auto& dense_vec = *features.dense_features();
    const auto& sparse_vec = *features.sparse_features();

    // Convert double to float for dense features
    std::vector<float> dense_floats;
    dense_floats.reserve(dense_vec.size());
    for (double d : dense_vec) {
      dense_floats.push_back(static_cast<float>(d));
    }

    // Calculate max number of inferences per thread
    int num_inferences_max =
        (total_num_inferences + args.cpu_threads_arg - 1)
        / args.cpu_threads_arg;

    // Run inference with client-provided features
    std::vector<folly::Future<int>> futures;
    for (int i = 0; i < args.cpu_threads_arg; i++) {
      int num_inferences = std::min(num_inferences_max, total_num_inferences);
      auto f = folly::via(
          this_thread.cpuThreadPool.get(),
          [num_inferences, batch_size, &dense_floats, &sparse_vec, &this_thread]() {
            return this_thread.dlrm_ranker->inferWithFeatures(
                dense_floats.data(),
                sparse_vec.data(),
                batch_size,
                num_inferences);
          });
      futures.push_back(std::move(f));
      total_num_inferences -= num_inferences;
      if (total_num_inferences <= 0) break;
    }
    auto fs = folly::collect(futures).get();
    result = std::accumulate(fs.begin(), fs.end(), 0);
  } else {
    // Fallback to server-side feature generation
    result = dlrmInferenceServerSideDataGeneration(
        this_thread, *request.num_inferences());
  }

  // Generate response (same as PageRankRequestHandler)
  auto per_thread_num_objects = args.num_objects_arg / args.srv_io_threads_arg;
  auto r = ranking::generators::generateRandomRankingResponse(per_thread_num_objects);
  ranking::RankingResponse resp = r;

  folly::IOBufQueue bufq;
  apache::thrift::CompactSerializer::serialize(resp, &bufq);
  auto buf = bufq.move();

  context.SendResponse(buf->data(), buf->length());
}
#endif // FEEDSIM_USE_DLRM

void PageRankRequestHandler(
    oldisim::NodeThread& thread,
    oldisim::QueryContext& context,
    std::vector<ThreadData>& thread_data) {
  auto& this_thread = thread_data[thread.get_thread_num()];
  search::PointerChase& chaser = *this_thread.pointer_chaser;

  // ICacheBuster stage (only for PageRank mode)
  if (g_workload_type == WorkloadType::PAGERANK) {
    const int min_iterations = std::max(args.min_icache_iterations_arg, 0);
    const int num_iterations =
        static_cast<int>(this_thread.latency_distribution(this_thread.rng)) +
        min_iterations;
    ICacheBuster& buster = *this_thread.icache_buster;

    for (int i = 0; i < num_iterations; i++) {
      buster.RunNextMethod();
    }
  }

  int result = 0;

  // Ranking stage - either PageRank or DLRM
  if (g_workload_type == WorkloadType::PAGERANK) {
    // PageRank workload
    auto per_thread_subset = args.graph_subset_arg / args.cpu_threads_arg;

    std::vector<folly::Future<int>> futures;
    for (int i = 0; i < args.cpu_threads_arg; i++) {
      auto f = folly::via(
          this_thread.cpuThreadPool.get(),
          [i, &this_thread, per_thread_subset]() {
            return this_thread.page_ranker->rank(
                i,
                args.graph_max_iters_arg,
                kPageRankThreshold,
                args.rank_trials_per_thread_arg,
                per_thread_subset);
          });
      futures.push_back(std::move(f));
    }
    auto fs = folly::collect(futures).get();
    result = std::accumulate(fs.begin(), fs.end(), 0);
  }
#ifdef FEEDSIM_USE_DLRM
  else if (g_workload_type == WorkloadType::DLRM) {
    result = dlrmInferenceServerSideDataGeneration(
        this_thread, args.dlrm_inferences_per_request_arg);
  }
#endif

  // I/O simulation stage
  auto timekeeper = this_thread.timekeeperPool->getTimekeeper();
  auto s = folly::futures::sleep(
               std::chrono::milliseconds(args.io_time_ms_arg), timekeeper.get())
               .via(this_thread.ioThreadPool.get())
               .thenValue([&](auto&& _) {
                 return result + 1;
               });
  result = std::move(s).get();

  auto compressed = compressPayload(this_thread.random_string, result);

  auto per_thread_num_objects = args.num_objects_arg / args.srv_io_threads_arg;

  std::vector<folly::Future<int>> compressionFutures;
  for (int i = 0; i < args.srv_io_threads_arg; i++) {
    auto f = folly::via(this_thread.srvIOThreadPool.get(), [&]() {
      auto resp = ranking::generators::generateRandomRankingResponse(
          per_thread_num_objects);
      auto payloadiobufq = serializePayload(resp);
      auto buf = payloadiobufq.move();
      const auto compress_length = buf->computeChainDataLength() / 2;
      auto total_size = 0;
      folly::IOBuf::Iterator it = buf->begin();
      while (it != buf->end() && total_size < compress_length) {
        const auto& b = *it;
        auto iobuf = folly::IOBuf::copyBuffer(b.data(), b.size());
        auto c = compressThrift(std::move(iobuf));
        total_size += b.size();
        ++it;
      }
      return 1;
    });
    compressionFutures.push_back(std::move(f));
  }
  auto cfs = folly::collect(compressionFutures).get();
  int cResult = std::accumulate(cfs.begin(), cfs.end(), 0);

  auto per_thread_chase_iterations =
      args.chase_iterations_arg / args.srv_threads_arg;
  std::vector<folly::Future<int>> chaseFutures;
  for (int i = 0; i < args.srv_threads_arg; i++) {
    auto f = folly::via(this_thread.srvCPUThreadPool.get(), [&]() {
      chaser.Chase(per_thread_chase_iterations);
      return 1;
    });
    chaseFutures.push_back(std::move(f));
  }
  auto chaseFs = folly::collect(chaseFutures).get();
  int chaseResult = std::accumulate(chaseFs.begin(), chaseFs.end(), 0);

  // Generate a response
  auto r = ranking::generators::generateRandomRankingResponse(
      per_thread_num_objects);
  ranking::RankingResponse resp = r;

  // Serialize into FBThrift
  auto payloadiobufq = serializePayload(resp);
  auto buf = payloadiobufq.move();

  auto uncompressed = decompressPayload(compressed);
  auto resp1 = deserializePayload(buf.get());

  context.SendResponse(buf->data(), buf->length());
}

int main(int argc, char** argv) {
  if (cmdline_parser(argc, argv, &args) != 0) {
    DIE("cmdline_parser failed"); // NOLINT
  }

  // Set logging level
  for (unsigned int i = 0; i < args.verbose_given; i++) {
    log_level = (log_level_t)(static_cast<int>(log_level) - 1);
  }
  if (args.quiet_given != 0u) {
    log_level = QUIET;
  }

  // Determine workload type
  std::string workload_type_str = args.workload_type_arg;
  if (workload_type_str == "dlrm") {
#ifdef FEEDSIM_USE_DLRM
    g_workload_type = WorkloadType::DLRM;
    std::cout << "Using DLRM workload type" << std::endl;
#else
    DIE("DLRM workload requested but FEEDSIM_USE_DLRM is not defined. "
        "Rebuild with LibTorch support.");
#endif
  } else {
    g_workload_type = WorkloadType::PAGERANK;
    std::cout << "Using PageRank workload type" << std::endl;
  }

  int fake_argc = 1;
  char* fake_argv[2] = {const_cast<char*>("./LeafNodeRank"), nullptr};
  char** sargv = static_cast<char**>(fake_argv);
  folly::init(&fake_argc, &sargv);
  auto cpuThreadPool =
      std::make_shared<folly::CPUThreadPoolExecutor>(args.cpu_threads_arg);

  auto srvCPUThreadPool = std::make_shared<folly::CPUThreadPoolExecutor>(
      args.srv_threads_arg,
      std::make_shared<folly::NamedThreadFactory>("srvCPUThread"));

  auto srvIOThreadPool = std::make_shared<folly::CPUThreadPoolExecutor>(
      args.srv_io_threads_arg,
      std::make_shared<folly::NamedThreadFactory>("srvIOThread"));

  auto ioThreadPool =
      std::make_shared<folly::IOThreadPoolExecutor>(args.io_threads_arg);

  auto timekeeperPool =
      std::make_shared<ranking::TimekeeperPool>(args.timekeeper_threads_arg);

  // Warm up all thread pools to ensure threads are spawned and ready
  // This prevents cold-start latency spikes during actual request processing
  std::cout << "Warming up thread pools..." << std::endl;
  {
    const int warmup_tasks = 100;  // Run multiple tasks to ensure all threads are active

    // Warm up CPU thread pool
    std::vector<folly::Future<int>> cpuFutures;
    for (int i = 0; i < warmup_tasks; i++) {
      cpuFutures.push_back(folly::via(cpuThreadPool.get(), []() {
        volatile int sum = 0;
        for (int j = 0; j < 1000; j++) sum += j;
        return static_cast<int>(sum);
      }));
    }
    folly::collectAll(std::move(cpuFutures)).get();

    // Warm up srvCPU thread pool
    std::vector<folly::Future<int>> srvCPUFutures;
    for (int i = 0; i < warmup_tasks; i++) {
      srvCPUFutures.push_back(folly::via(srvCPUThreadPool.get(), []() {
        volatile int sum = 0;
        for (int j = 0; j < 1000; j++) sum += j;
        return static_cast<int>(sum);
      }));
    }
    folly::collectAll(std::move(srvCPUFutures)).get();

    // Warm up srvIO thread pool
    std::vector<folly::Future<int>> srvIOFutures;
    for (int i = 0; i < warmup_tasks; i++) {
      srvIOFutures.push_back(folly::via(srvIOThreadPool.get(), []() {
        volatile int sum = 0;
        for (int j = 0; j < 1000; j++) sum += j;
        return static_cast<int>(sum);
      }));
    }
    folly::collectAll(std::move(srvIOFutures)).get();

    // Warm up IO thread pool (uses different API)
    std::vector<folly::Future<int>> ioFutures;
    for (int i = 0; i < warmup_tasks; i++) {
      ioFutures.push_back(folly::via(ioThreadPool.get(), []() {
        return 1;
      }));
    }
    folly::collectAll(std::move(ioFutures)).get();

    // Warm up timekeeper by scheduling a few sleeps
    auto timekeeper = timekeeperPool->getTimekeeper();
    std::vector<folly::SemiFuture<folly::Unit>> sleepFutures;
    for (int i = 0; i < 10; i++) {
      sleepFutures.push_back(
          folly::futures::sleep(std::chrono::milliseconds(1), timekeeper.get()));
    }
    folly::collectAll(std::move(sleepFutures)).get();
  }
  std::cout << "Thread pool warmup complete" << std::endl;

  std::vector<ThreadData> thread_data(args.threads_arg);
  ranking::dwarfs::PageRankParams params{
      args.graph_scale_arg, args.graph_degree_arg};

#ifdef FEEDSIM_USE_DLRM
  // Initialize shared DLRM model if using DLRM workload
  std::shared_ptr<ranking::dwarfs::DLRM> shared_dlrm_ranker;
  if (g_workload_type == WorkloadType::DLRM) {
    if (!args.dlrm_model_path_given) {
      DIE("DLRM workload requires --dlrm_model_path");
    }
    ranking::dwarfs::DLRMParams dlrm_params;
    dlrm_params.model_path = args.dlrm_model_path_arg;
    dlrm_params.batch_size = args.dlrm_batch_size_arg;
    dlrm_params.num_threads = args.dlrm_threads_arg;

    unsigned dlrm_seed = 0;
    if (args.dlrm_seed_given) {
      dlrm_seed = static_cast<unsigned>(args.dlrm_seed_arg);
    }

    // Create shared DLRM model (thread-safe for inference)
    shared_dlrm_ranker = std::make_shared<ranking::dwarfs::DLRM>(
        dlrm_params, args.threads_arg, dlrm_seed);

    // Warm up DLRM model to stabilize inference latency
    // JIT compilation and memory allocation happen on first few inferences
    std::cout << "Warming up DLRM model..." << std::endl;
    const int warmup_iterations = 10;  // Run enough iterations to JIT compile all paths
    for (int i = 0; i < warmup_iterations; i++) {
      shared_dlrm_ranker->infer(1, args.dlrm_batch_size_arg);
    }
    std::cout << "DLRM warmup complete (" << warmup_iterations << " iterations)" << std::endl;
  }
#endif

  // create or load a graph (only for PageRank mode)
  if (g_workload_type == WorkloadType::PAGERANK) {
    if (args.load_graph_given) {
      if (args.instrument_graph_given) {
        auto start_load = std::chrono::steady_clock::now();
        g_shared_graph = params.loadGraphFromFile(args.load_graph_arg);
        auto end_load = std::chrono::steady_clock::now();
        auto load_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                end_load - start_load)
                .count();
        std::cout << "Graph loading time: " << load_duration << " ms"
                  << std::endl;
      } else {
        g_shared_graph = params.loadGraphFromFile(args.load_graph_arg);
      }
    } else {
      if (args.instrument_graph_given) {
        auto start_build = std::chrono::steady_clock::now();
        g_shared_graph = params.buildGraph();
        auto end_build = std::chrono::steady_clock::now();
        auto build_duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                end_build - start_build)
                .count();
        std::cout << "Graph building time: " << build_duration << " ms"
                  << std::endl;

        if (args.store_graph_given) {
          auto start_store = std::chrono::steady_clock::now();
          params.storeGraphToFile(g_shared_graph, args.store_graph_arg);
          auto end_store = std::chrono::steady_clock::now();
          auto store_duration =
              std::chrono::duration_cast<std::chrono::milliseconds>(
                  end_store - start_store)
                  .count();
          std::cout << "Graph storing time: " << store_duration << " ms"
                    << std::endl;
        }
      } else {
        g_shared_graph = params.buildGraph();
        if (args.store_graph_given) {
          params.storeGraphToFile(g_shared_graph, args.store_graph_arg);
        }
      }
    }
  }

  oldisim::LeafNodeServer server(args.port_arg);
  server.SetThreadStartupCallback([&](auto&& thread) {
#ifdef FEEDSIM_USE_DLRM
    return ThreadStartup(
        thread,
        thread_data,
        params,
        cpuThreadPool,
        srvCPUThreadPool,
        srvIOThreadPool,
        ioThreadPool,
        timekeeperPool,
        shared_dlrm_ranker);
#else
    return ThreadStartup(
        thread,
        thread_data,
        params,
        cpuThreadPool,
        srvCPUThreadPool,
        srvIOThreadPool,
        ioThreadPool,
        timekeeperPool);
#endif
  });

  // Choose request handler based on async_io flag
  if (args.async_io_given) {
    std::cout << "Using ASYNC (non-blocking) I/O mode - eliminates thread starvation" << std::endl;
    std::cout << "  I/O latency distribution: " << args.io_latency_distribution_arg << std::endl;
    std::cout << "  I/O latency mean: " << args.io_latency_mean_ms_arg << " ms" << std::endl;
    if (std::string(args.io_latency_distribution_arg) == "lognormal") {
      std::cout << "  I/O latency stddev: " << args.io_latency_stddev_ms_arg << " ms" << std::endl;
    }
    if (args.io_stages_arg > 1) {
      std::cout << "  I/O stages: " << args.io_stages_arg << " x " << args.io_stage_latency_ms_arg << " ms" << std::endl;
    }

    server.RegisterQueryCallback(
        ranking::kPageRankRequestType,
        [&thread_data](auto&& thread, auto&& context) {
          return AsyncPageRankRequestHandler(thread, context, thread_data);
        });
  } else {
    std::cout << "Using BLOCKING I/O mode (original behavior)" << std::endl;
    server.RegisterQueryCallback(
        ranking::kPageRankRequestType,
        [&thread_data](auto&& thread, auto&& context) {
          return PageRankRequestHandler(thread, context, thread_data);
        });
  }

#ifdef FEEDSIM_USE_DLRM
  // Phase 7: Register DLRM request handler for client-side feature generation
  if (g_workload_type == WorkloadType::DLRM) {
    std::cout << "Registering DLRM request handler for client-side features" << std::endl;
    server.RegisterQueryCallback(
        ranking::kDLRMRequestType,
        [&thread_data](auto&& thread, auto&& context) {
          return DLRMRequestHandler(thread, context, thread_data);
        });
  }
#endif
  server.SetNumThreads(args.threads_arg);
  server.SetThreadPinning(args.noaffinity_given == 0u);
  server.SetThreadLoadBalancing(args.noloadbalance_given == 0u);

  server.EnableMonitoring(args.monitor_port_arg);

  server.Run();

  return 0;
}

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

// ManagedCompression is the documented Meta standard for application-level
// compression (per fbcode/.llms/rules/managed_compression.md), but it is
// internal-only. The benchpress repo is open-sourced, so the include and
// usages below are gated behind BENCHPRESS_INTERNAL. When the gate is not
// defined (the current OSS / CMake build path) we fall back to raw folly
// ZSTD, which is the historical behavior. The gate is wired up by the
// fbcode-internal Buck build (see packages/feedsim/.../mock_services/BUCK
// for the analogous internal-only wiring); the open-source CMake build
// leaves it undefined.
#ifdef BENCHPRESS_INTERNAL
#include <folly/Singleton.h>
#include "common/managed_compression/ManagedCompression.h"
#endif

#include <folly/executors/GlobalExecutor.h>
#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>
#include <folly/init/Init.h>
#include <folly/system/HardwareConcurrency.h>

#include <thrift/lib/cpp2/protocol/CompactProtocol.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include "FeedSimServer.h"
#include "FeedSimProtocol.h"

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

#include "SilesiaLoader.h"
#include "MockServicesClient.h"
#include "RpcDistRegistry.h"
#include "generators/RankingGenerators.h"
#include "generators/SilesiaResponseGenerator.h"

#include "feature_extractors/FeatureExtractorSuite.h"
#include "feature_extractors/HashLookupExtractor.h"
#include "feature_extractors/FeatureLayoutExtractor.h"
#include "feature_extractors/EmbeddingLookupExtractor.h"
#include "feature_extractors/ContainerExtractor.h"
#include "feature_extractors/TreeTraversalExtractor.h"
#include "feature_extractors/BitsetExtractor.h"
#include "feature_extractors/generated/registry.h"

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
  // Phase 4 thread-pool aliases (names kept for callsite stability):
  //   cpuThreadPool       -> folly::getGlobalCPUExecutor() ("GlobalCPUThread")
  //   srvCPUThreadPool    -> RANKER pool (NamedThreadFactory("RANKER"))
  //   srvIOThreadPool     -> legacy compression pool (kept until Phase 6)
  //   ioThreadPool        -> ThriftSrv.IO pool (NamedThreadFactory("ThriftSrv.IO"))
  //   srEventBasePool     -> NEW outbound-RPC EventBase pool (idle in Phase 4)
  std::shared_ptr<folly::Executor> cpuThreadPool;
  std::shared_ptr<folly::CPUThreadPoolExecutor> srvCPUThreadPool;
  std::shared_ptr<folly::CPUThreadPoolExecutor> srvIOThreadPool;
  std::shared_ptr<folly::IOThreadPoolExecutor> ioThreadPool;
  std::shared_ptr<folly::IOThreadPoolExecutor> srEventBasePool;
  std::shared_ptr<ranking::TimekeeperPool> timekeeperPool;

  // Phase 5: outbound RPC fanout to mock_services. Populated only when
  // --rpc_dist_path is set; nullptr otherwise (in which case the legacy
  // folly::futures::sleep path is used).
  //
  // mock_client is per-thread because each MockServiceAsyncClient is
  // pinned to one folly::EventBase (see MockServicesClient.h). registry
  // and silesia are shared (read-only after load) — held here as raw
  // pointers to globals to avoid shared_ptr churn on the request path.
  ranking::RpcDistRegistry* rpc_registry = nullptr;
  ranking::SilesiaLoader* rpc_silesia = nullptr;
  std::unique_ptr<ranking::MockServicesClient> mock_client;
  std::mt19937 rpc_rng;
  // rpc_rng is sampled from issueOutboundFanout which runs inside .thenValue
  // continuations on the multi-threaded ioThreadPool — multiple concurrent
  // pipelines can hit the same ThreadData. mt19937 is not thread-safe, so
  // serialize with this mutex.
  std::mutex rpc_rng_mutex;
  std::unique_ptr<ranking::dwarfs::PageRank> page_ranker;
#ifdef FEEDSIM_USE_DLRM
  std::shared_ptr<ranking::dwarfs::DLRM> dlrm_ranker;
#endif
  std::unique_ptr<search::PointerChase> pointer_chaser;
  std::unique_ptr<ICacheBuster> icache_buster;
  std::default_random_engine rng;
  std::gamma_distribution<double> latency_distribution;
  std::string random_string;

  // Feature extraction suite (Phase 2)
  std::unique_ptr<FeatureExtractorSuite> feature_suite;

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

// Server-side Silesia corpus + response generator. When --silesia_dir is
// given, response generation pulls bytes from the corpus instead of running
// xor128() RNG, removing ~15% of CPU that would otherwise be wasted on RNG
// (no production analog).
static std::unique_ptr<ranking::SilesiaLoader> g_silesia_loader;
static std::unique_ptr<ranking::generators::SilesiaResponseGenerator>
    g_silesia_response_gen;

// Phase 5: process-wide RPC fanout state. Loaded once in main() if
// --rpc_dist_path is set; nullptr otherwise. ThreadStartup grabs raw
// pointers into ThreadData so the request path doesn't pay for atomic
// shared_ptr ops on every call.
static std::unique_ptr<ranking::RpcDistRegistry> g_rpc_registry;
// Reused for fanout request body padding when --silesia_dir is also set.
// When --silesia_dir is unset, fanout request bodies are zero-filled.
static ranking::SilesiaLoader* g_rpc_silesia = nullptr;

// Helper that returns either a Silesia-generated or RNG-generated
// RankingResponse, depending on whether --silesia_dir was provided.
static ranking::RankingResponse generateResponse(int num_objects) {
  if (g_silesia_response_gen) {
    return g_silesia_response_gen->generateRankingResponse(num_objects);
  }
  return ranking::generators::generateRandomRankingResponse(num_objects);
}

#ifdef FEEDSIM_USE_DLRM
void ThreadStartup(
    int thread_id,
    std::vector<ThreadData>& thread_data,
    ranking::dwarfs::PageRankParams& params,
    const std::shared_ptr<folly::Executor>& cpuThreadPool,
    const std::shared_ptr<folly::CPUThreadPoolExecutor>& srvCPUThreadPool,
    const std::shared_ptr<folly::CPUThreadPoolExecutor>& srvIOThreadPool,
    const std::shared_ptr<folly::IOThreadPoolExecutor>& ioThreadPool,
    const std::shared_ptr<folly::IOThreadPoolExecutor>& srEventBasePool,
    const std::shared_ptr<ranking::TimekeeperPool>& timekeeperPool,
    const std::shared_ptr<ranking::dwarfs::DLRM>& shared_dlrm_ranker) {
  auto& this_thread = thread_data[thread_id];
  this_thread.cpuThreadPool = cpuThreadPool;
  this_thread.srvCPUThreadPool = srvCPUThreadPool;
  this_thread.srvIOThreadPool = srvIOThreadPool;
  this_thread.ioThreadPool = ioThreadPool;
  this_thread.srEventBasePool = srEventBasePool;
  this_thread.timekeeperPool = timekeeperPool;

  // Phase 5: populate per-thread RPC fanout state. registry / silesia
  // pointers are global (initialized in main() if --rpc_dist_path was
  // set); mock_client is constructed lazily here so it lives on a
  // thread from the SREventBase pool. Seed the fanout RNG with
  // hardware_destructive seed mixing so each thread gets independent
  // sample sequences without sharing the std::default_random_engine
  // used elsewhere in this struct.
  this_thread.rpc_registry = g_rpc_registry.get();
  this_thread.rpc_silesia = g_rpc_silesia;
  this_thread.rpc_rng.seed(
      std::random_device{}() ^ static_cast<unsigned>(thread_id + 1));
  if (this_thread.rpc_registry != nullptr && srEventBasePool != nullptr) {
    auto* evb = srEventBasePool->getEventBase();
    try {
      this_thread.mock_client =
          std::make_unique<ranking::MockServicesClient>(
              evb,
              args.mock_services_host_arg,
              static_cast<uint16_t>(args.mock_services_port_arg));
    } catch (const std::exception& e) {
      std::cerr << "Failed to connect to mock_services on "
                << args.mock_services_host_arg << ":"
                << args.mock_services_port_arg
                << " (thread " << thread_id << "): " << e.what()
                << ". Falling back to legacy folly::futures::sleep path."
                << std::endl;
      this_thread.mock_client.reset();
    }
  }

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
  }

  // ICacheBuster only for PAGERANK workload (used by PageRank request handlers)
  if (g_workload_type == WorkloadType::PAGERANK) {
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

  // Initialize feature extraction suite if enabled
  if (args.feature_extractors_given) {
    this_thread.feature_suite = std::make_unique<FeatureExtractorSuite>();
    // Add 6 hand-written extractors
    this_thread.feature_suite->addExtractor(
        std::make_unique<HashLookupExtractor>());
    this_thread.feature_suite->addExtractor(
        std::make_unique<FeatureLayoutExtractor>());
    this_thread.feature_suite->addExtractor(
        std::make_unique<EmbeddingLookupExtractor>());
    this_thread.feature_suite->addExtractor(
        std::make_unique<ContainerExtractor>());
    this_thread.feature_suite->addExtractor(
        std::make_unique<TreeTraversalExtractor>());
    this_thread.feature_suite->addExtractor(
        std::make_unique<BitsetExtractor>());
    // Add generated extractors (1035 variants, each dispatches to 1 of 1000 copies)
    auto generated = dcperf::feature_extractors::generated::createGeneratedExtractors();
    for (auto& ext : generated) {
      this_thread.feature_suite->addExtractor(std::move(ext));
    }
    this_thread.feature_suite->initializeAll(
        args.feature_complexity_arg, noderank_seed);
    this_thread.feature_suite->initializeFlatDispatch(noderank_seed);
  }

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
    int thread_id,
    std::vector<ThreadData>& thread_data,
    ranking::dwarfs::PageRankParams& params,
    const std::shared_ptr<folly::Executor>& cpuThreadPool,
    const std::shared_ptr<folly::CPUThreadPoolExecutor>& srvCPUThreadPool,
    const std::shared_ptr<folly::CPUThreadPoolExecutor>& srvIOThreadPool,
    const std::shared_ptr<folly::IOThreadPoolExecutor>& ioThreadPool,
    const std::shared_ptr<folly::IOThreadPoolExecutor>& srEventBasePool,
    const std::shared_ptr<ranking::TimekeeperPool>& timekeeperPool) {
  auto& this_thread = thread_data[thread_id];
  auto graph = params.makeGraphCopy(g_shared_graph);
  this_thread.cpuThreadPool = cpuThreadPool;
  this_thread.srvCPUThreadPool = srvCPUThreadPool;
  this_thread.srvIOThreadPool = srvIOThreadPool;
  this_thread.ioThreadPool = ioThreadPool;
  this_thread.srEventBasePool = srEventBasePool;
  this_thread.timekeeperPool = timekeeperPool;

  // Phase 5: populate per-thread RPC fanout state. registry / silesia
  // pointers are global (initialized in main() if --rpc_dist_path was
  // set); mock_client is constructed lazily here so it lives on a
  // thread from the SREventBase pool. Seed the fanout RNG with
  // hardware_destructive seed mixing so each thread gets independent
  // sample sequences without sharing the std::default_random_engine
  // used elsewhere in this struct.
  this_thread.rpc_registry = g_rpc_registry.get();
  this_thread.rpc_silesia = g_rpc_silesia;
  this_thread.rpc_rng.seed(
      std::random_device{}() ^ static_cast<unsigned>(thread_id + 1));
  if (this_thread.rpc_registry != nullptr && srEventBasePool != nullptr) {
    auto* evb = srEventBasePool->getEventBase();
    try {
      this_thread.mock_client =
          std::make_unique<ranking::MockServicesClient>(
              evb,
              args.mock_services_host_arg,
              static_cast<uint16_t>(args.mock_services_port_arg));
    } catch (const std::exception& e) {
      std::cerr << "Failed to connect to mock_services on "
                << args.mock_services_host_arg << ":"
                << args.mock_services_port_arg
                << " (thread " << thread_id << "): " << e.what()
                << ". Falling back to legacy folly::futures::sleep path."
                << std::endl;
      this_thread.mock_client.reset();
    }
  }
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
  // ICacheBuster only for PAGERANK workload
  if (g_workload_type == WorkloadType::PAGERANK) {
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

  // Initialize feature extraction suite if enabled
  if (args.feature_extractors_given) {
    this_thread.feature_suite = std::make_unique<FeatureExtractorSuite>();
    // Add 6 hand-written extractors
    this_thread.feature_suite->addExtractor(
        std::make_unique<HashLookupExtractor>());
    this_thread.feature_suite->addExtractor(
        std::make_unique<FeatureLayoutExtractor>());
    this_thread.feature_suite->addExtractor(
        std::make_unique<EmbeddingLookupExtractor>());
    this_thread.feature_suite->addExtractor(
        std::make_unique<ContainerExtractor>());
    this_thread.feature_suite->addExtractor(
        std::make_unique<TreeTraversalExtractor>());
    this_thread.feature_suite->addExtractor(
        std::make_unique<BitsetExtractor>());
    // Add generated extractors (1035 variants, each dispatches to 1 of 1000 copies)
    auto generated = dcperf::feature_extractors::generated::createGeneratedExtractors();
    for (auto& ext : generated) {
      this_thread.feature_suite->addExtractor(std::move(ext));
    }
    this_thread.feature_suite->initializeAll(
        args.feature_complexity_arg, noderank_seed);
    this_thread.feature_suite->initializeFlatDispatch(noderank_seed);
  }

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

#ifdef BENCHPRESS_INTERNAL
namespace {
using facebook::managed_compression::ManagedCompressionFactory;

// One ManagedCompressionFactory per (oncall, project) pair, lifetime =
// process. Per the ManagedCompression skill / wiki, constructing a new
// factory per call is expensive and explicitly discouraged.
//
// Two categories are used in this file:
//   "leaf_random_string"   — the pseudo-random payload bytes shared by
//                            compressPayload / decompressPayload. Both
//                            sides MUST use the same category so
//                            ManagedCompression can serve the right
//                            dictionary on decompress.
//   "leaf_thrift_payload"  — serialized RankingResponse (CompactProtocol)
//                            consumed by compressThrift.
class FeedSimCompressionTag {};
folly::Singleton<ManagedCompressionFactory, FeedSimCompressionTag> gFactory(
    [] {
      return new ManagedCompressionFactory(
          /*oncall_team=*/"chips_dcperf",
          /*project=*/"feedsim");
    });

std::shared_ptr<folly::compression::Codec> getRandomStringCodec() {
  // getCachedCodec() reuses the codec instance per category; preferred
  // over getCodec() for hot paths per references/cpp.md. try_get() can
  // return null before SingletonVault::registrationComplete() or after
  // destroyInstances() during shutdown — fail loudly rather than crash
  // with a null-deref.
  auto factory = gFactory.try_get();
  CHECK(factory) << "ManagedCompressionFactory singleton unavailable";
  return factory->getCachedCodec("leaf_random_string");
}

std::shared_ptr<folly::compression::Codec> getThriftPayloadCodec() {
  auto factory = gFactory.try_get();
  CHECK(factory) << "ManagedCompressionFactory singleton unavailable";
  return factory->getCachedCodec("leaf_thrift_payload");
}
} // namespace
#endif // BENCHPRESS_INTERNAL

std::string compressPayload(const std::string& data, int /*result*/) {
  folly::StringPiece output(
      data.data(),
      std::min(args.compression_data_size_arg, args.random_data_size_arg));
#ifdef BENCHPRESS_INTERNAL
  return getRandomStringCodec()->compress(output);
#else
  auto codec =
      folly::compression::getCodec(folly::compression::CodecType::ZSTD);
  std::string compressed = codec->compress(output);
  return std::move(compressed);
#endif
}

std::string decompressPayload(const std::string& data) {
#ifdef BENCHPRESS_INTERNAL
  return getRandomStringCodec()->uncompress(data);
#else
  auto codec =
      folly::compression::getCodec(folly::compression::CodecType::ZSTD);
  std::string decompressed = codec->uncompress(data);
  return decompressed;
#endif
}

std::unique_ptr<folly::IOBuf> compressThrift(
    std::unique_ptr<folly::IOBuf> buf) {
#ifdef BENCHPRESS_INTERNAL
  return getThriftPayloadCodec()->compress(buf.get());
#else
  auto codec =
      folly::compression::getCodec(folly::compression::CodecType::ZSTD);
  auto compressed_buf = codec->compress(buf.get());
  return compressed_buf;
#endif
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

// ============================================================================
// Phase 5: outbound RPC fanout to mock_services.
//
// When --rpc_dist_path is set, request handlers replace
// folly::futures::sleep(io_latency_ms) with a real fanout of Thrift RPCs to
// a co-located mock_services Thrift server. Per-method call counts are
// calibrated from production (ranking::perSessionCounts(), see
// ~/feedsim_v2/docs/phase5_researcher_notes.md §4) and scaled by
// --rpc_fanout_scale (default 0.025, ~94 RPCs/session).
//
// Each RPC carries a payload sampled from the request_size percentile
// distribution; the first 4 bytes are a big-endian uint32_t encoding the
// desired response_size sampled from the response_size percentile
// distribution (see mock_services/MockService.thrift wire contract). The
// server uses that header to size its response, so client and server stay
// in sync without an out-of-band agreement.
// ============================================================================

namespace {

// Build a single RPC request body of `req_size` bytes:
//   [4 bytes: big-endian response_size][padding bytes]
// The padding is zero-filled if no Silesia corpus is available; otherwise
// it is sliced from a random Silesia file (cheap mmap-backed read).
std::string buildFanoutRequest(
    size_t req_size,
    uint32_t response_size,
    ranking::SilesiaLoader* silesia,
    std::mt19937& rng) {
  // Always reserve at least 4 bytes for the header.
  size_t actual_size = std::max<size_t>(req_size, sizeof(uint32_t));
  std::string buf;
  buf.resize(actual_size);
  ranking::writeBigEndianResponseSize(buf.data(), response_size);

  size_t padding = actual_size - sizeof(uint32_t);
  if (padding == 0) {
    return buf;
  }
  if (silesia != nullptr && silesia->isLoaded()) {
    char* dst = buf.data() + sizeof(uint32_t);
    size_t remaining = padding;
    while (remaining > 0) {
      const uint8_t* snippet = nullptr;
      size_t snippet_size = 0;
      std::string filename;
      silesia->getRandomSnippet(
          rng, /*min_size=*/1, remaining, snippet, snippet_size, filename);
      if (snippet_size == 0) {
        std::memset(dst, 0, remaining);
        break;
      }
      std::memcpy(dst, snippet, snippet_size);
      dst += snippet_size;
      remaining -= snippet_size;
    }
  } else {
    std::memset(buf.data() + sizeof(uint32_t), 0, padding);
  }
  return buf;
}

} // namespace

// Issue the full per-session outbound RPC fanout. Iterates the 20
// outbound methods, computes how many calls each gets at `scale`,
// samples request_size / response_size / latency_us per call, and
// dispatches via the per-thread MockServicesClient.
//
// Returns a Future<int> that resolves to the total number of completed
// RPCs once all are done (regardless of success — we count attempts so
// caller code can keep the same shape as before).
static folly::Future<int> issueOutboundFanout(
    ThreadData& td, double scale) {
  if (td.mock_client == nullptr || td.rpc_registry == nullptr) {
    // Defensive: caller should have checked --rpc_dist_path.
    return folly::makeFuture<int>(0);
  }

  std::vector<folly::Future<int>> futs;
  futs.reserve(128);

  for (size_t i = 0; i < ranking::kNumMethods; ++i) {
    auto m = static_cast<ranking::MethodIdx>(i);
    int n = std::max(
        1,
        static_cast<int>(
            std::round(ranking::perSessionCounts()[i] * scale)));

    const auto& req_sampler = td.rpc_registry->requestSize(m);
    const auto& resp_sampler = td.rpc_registry->responseSize(m);
    const auto& lat_sampler = td.rpc_registry->latencyUs(m);

    for (int k = 0; k < n; ++k) {
      size_t req_size;
      uint32_t resp_size;
      int32_t lat_us;
      // Sampling and buildFanoutRequest both mutate rpc_rng; serialize
      // to avoid concurrent-pipeline data race on RNG state.
      std::string req;
      {
        std::lock_guard<std::mutex> lock(td.rpc_rng_mutex);
        req_size = req_sampler.sample(td.rpc_rng);
        resp_size = static_cast<uint32_t>(resp_sampler.sample(td.rpc_rng));
        lat_us = static_cast<int32_t>(lat_sampler.sampleI64(td.rpc_rng));
        req = buildFanoutRequest(
            req_size, resp_size, td.rpc_silesia, td.rpc_rng);
      }

      futs.push_back(td.mock_client
                         ->dispatchByEnum(m, req, lat_us)
                         .via(td.srEventBasePool.get())
                         .thenValue([](std::string&&) { return 1; })
                         .thenError(
                             folly::tag_t<std::exception>{},
                             [](const std::exception&) { return 0; }));
    }
  }

  return folly::collectAll(std::move(futs))
      .via(td.srEventBasePool.get())
      .thenValue([](std::vector<folly::Try<int>> results) {
        int total = 0;
        for (auto& r : results) {
          if (r.hasValue()) total += r.value();
        }
        return total;
      });
}

// Drop-in replacement for folly::futures::sleep at the I/O simulation
// callsites in the request handlers. When --rpc_dist_path is set,
// dispatches the full per-session outbound fanout (94 RPCs at default
// scale=0.025) instead of sleeping. When unset, falls back to the legacy
// folly::futures::sleep so the previous behavior is preserved verbatim
// (regression-safety A/B path).
//
// Returns Future<folly::Unit> so the existing .thenValue([... ](folly::Unit)
// continuations need no change.
static folly::Future<folly::Unit> simulateIoOrFanout(
    ThreadData& td,
    int io_latency_ms,
    folly::Timekeeper* tk,
    folly::Executor* via_executor) {
  if (td.mock_client != nullptr && td.rpc_registry != nullptr) {
    return issueOutboundFanout(td, args.rpc_fanout_scale_arg)
        .via(via_executor)
        .thenValue([](int /*completed*/) { return folly::unit; });
  }
  return folly::futures::sleep(std::chrono::milliseconds(io_latency_ms), tk)
      .via(via_executor)
      .thenValue([](folly::Unit) { return folly::unit; });
}

#ifdef FEEDSIM_USE_DLRM
// Distribute `total_num_inferences` across cpu_threads_arg fan-out
// futures and return how many inferences each shard should run. The last
// shard absorbs any remainder so the sum equals total_num_inferences.
static std::vector<int> shardInferences(int total_num_inferences) {
  std::vector<int> shards;
  int per_shard =
      (total_num_inferences + args.cpu_threads_arg - 1) / args.cpu_threads_arg;
  for (int i = 0; i < args.cpu_threads_arg && total_num_inferences > 0; ++i) {
    int n = std::min(per_shard, total_num_inferences);
    shards.push_back(n);
    total_num_inferences -= n;
  }
  return shards;
}

// Run DLRM inference where features are generated INSIDE DLRM::infer (server
// side). Returns a future so the caller can chain the rest of the request
// pipeline asynchronously instead of blocking.
static folly::Future<int> dlrmInferenceServerSide(
    ThreadData& this_thread, int total_num_inferences) {
  int batch_size = args.dlrm_batch_size_arg;
  std::vector<folly::Future<int>> futures;
  for (int n : shardInferences(total_num_inferences)) {
    futures.push_back(folly::via(
        this_thread.cpuThreadPool.get(),
        [n, batch_size, &this_thread]() {
          return this_thread.dlrm_ranker->infer(n, batch_size);
        }));
  }
  return folly::collect(std::move(futures))
      .via(this_thread.cpuThreadPool.get())
      .thenValue([](std::vector<int>&& results) {
        return std::accumulate(results.begin(), results.end(), 0);
      });
}

// Run DLRM inference where dense+sparse features are provided by the client
// (DLRM::inferWithFeatures). The client_features pointer must outlive the
// returned future — typically the caller keeps it alive in a shared_ptr.
static folly::Future<int> dlrmInferenceClientSide(
    ThreadData& this_thread,
    std::shared_ptr<std::vector<float>> dense_features,
    std::shared_ptr<std::vector<int64_t>> sparse_features,
    int batch_size,
    int total_num_inferences) {
  std::vector<folly::Future<int>> futures;
  for (int n : shardInferences(total_num_inferences)) {
    futures.push_back(folly::via(
        this_thread.cpuThreadPool.get(),
        [n, batch_size, &this_thread, dense_features, sparse_features]() {
          return this_thread.dlrm_ranker->inferWithFeatures(
              dense_features->data(),
              sparse_features->data(),
              batch_size,
              n);
        }));
  }
  return folly::collect(std::move(futures))
      .via(this_thread.cpuThreadPool.get())
      .thenValue([](std::vector<int>&& results) {
        return std::accumulate(results.begin(), results.end(), 0);
      });
}
#endif

/**
 * Populate feature inputs from story content bytes.
 *
 * Derives dense and sparse features from raw content to make feature
 * extraction data-dependent on real corpus data rather than random values.
 *
 * - Dense features: byte frequency histogram of content (256 bins -> 128 features)
 * - Sparse features: rolling hash of content bigrams -> embedding table indices
 * - CopyContext structData: populated from content bytes cast to floats
 */
static void populateFromStories(
    const std::vector<std::string>& story_contents,
    std::vector<float>& out_dense,
    std::vector<int64_t>& out_sparse,
    float* struct_data,
    int struct_size) {
  const int num_dense = 128;
  const int num_sparse = 64;
  out_dense.resize(num_dense, 0.0f);
  out_sparse.resize(num_sparse, 0);

  // Byte frequency histogram across all stories -> dense features
  uint32_t freq[256] = {};
  size_t total_bytes = 0;
  for (const auto& content : story_contents) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(content.data());
    size_t len = content.size();
    for (size_t i = 0; i < len; ++i) {
      freq[bytes[i]]++;
    }
    total_bytes += len;
  }

  // Map 256 bins to 128 dense features (pair adjacent bins)
  float inv_total = (total_bytes > 0) ? (1.0f / static_cast<float>(total_bytes)) : 0.0f;
  for (int i = 0; i < num_dense; ++i) {
    out_dense[i] = static_cast<float>(freq[2 * i] + freq[2 * i + 1]) * inv_total;
  }

  // Rolling hash of bigrams -> sparse feature indices
  int sparse_idx = 0;
  for (const auto& content : story_contents) {
    if (sparse_idx >= num_sparse) break;
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(content.data());
    size_t len = content.size();
    uint64_t h = 0x5BD1E995ULL;
    for (size_t i = 0; i + 1 < len && sparse_idx < num_sparse; i += 64) {
      h = h * 0x9E3779B97F4A7C15ULL;
      h ^= (static_cast<uint64_t>(bytes[i]) << 8) | bytes[i + 1];
      h ^= h >> 17;
      out_sparse[sparse_idx++] = static_cast<int64_t>(h % 50000);
    }
  }

  // Fill structData from content bytes (cast to float, normalized)
  int data_idx = 0;
  for (const auto& content : story_contents) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(content.data());
    size_t len = content.size();
    for (size_t i = 0; i < len && data_idx < struct_size; ++i) {
      struct_data[data_idx++] = static_cast<float>(bytes[i]) / 255.0f;
    }
  }
}

// Run feature extraction pipeline if enabled
// Uses flat dispatch: all 1.035M copy functions are shuffled into one vector
// and iterated sequentially. total_calls = num_stories * extractors_per_story.
// If story_contents is non-empty, features are derived from story bytes.
static void runFeatureExtraction(
    ThreadData& this_thread,
    const std::vector<std::string>& story_contents = {}) {
  if (!this_thread.feature_suite || this_thread.feature_suite->size() == 0) {
    return;
  }
  const int num_dense = 128;
  const int num_sparse = 64;
  const int total_calls = args.num_stories_arg * args.extractors_per_story_arg;

  std::vector<float> input_dense(num_dense);
  std::vector<int64_t> input_sparse(num_sparse);

  if (!story_contents.empty()) {
    // Derive features from story content (data-dependent on real corpus)
    // Use a temporary structData buffer for population
    std::vector<float> temp_struct(512);
    populateFromStories(
        story_contents, input_dense, input_sparse,
        temp_struct.data(), static_cast<int>(temp_struct.size()));

    // Run extractors with story-derived data and story content pointer
    // Concatenate story bytes for CopyContext
    std::vector<uint8_t> all_content;
    for (const auto& c : story_contents) {
      all_content.insert(all_content.end(), c.begin(), c.end());
    }
    this_thread.feature_suite->runFlatExtractors(
        total_calls, input_dense, input_sparse,
        all_content.data(), static_cast<int>(all_content.size()));
  } else {
    // Original random data path
    std::uniform_real_distribution<float> dense_dist(0.0f, 1.0f);
    std::uniform_int_distribution<int64_t> sparse_dist(0, 1000000);

    for (int i = 0; i < num_dense; ++i) {
      input_dense[i] = dense_dist(this_thread.rng);
    }
    for (int i = 0; i < num_sparse; ++i) {
      input_sparse[i] = sparse_dist(this_thread.rng);
    }

    this_thread.feature_suite->runFlatExtractors(
        total_calls, input_dense, input_sparse);
  }
}

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
 * CRITICAL: The RequestContext is moved into a shared_ptr to extend its lifetime
 * beyond the handler return. The server framework destroys the context after
 * the handler returns, but we need it to survive until the async callback.
 */
void AsyncPageRankRequestHandler(
    int thread_id,
    feedsim::RequestContext& context,
    std::vector<ThreadData>& thread_data) {
  auto& this_thread = thread_data[thread_id];

  // Move the RequestContext into a shared_ptr to extend its lifetime
  // beyond the handler return for async work.
  auto context_ptr = std::make_shared<feedsim::RequestContext>(std::move(context));

  // Stage 1: ICacheBuster (synchronous, adds I-cache pressure) - PAGERANK only
  if (this_thread.icache_buster) {
    const int min_iterations = std::max(args.min_icache_iterations_arg, 0);
    const int num_iterations =
        static_cast<int>(this_thread.latency_distribution(this_thread.rng)) +
        min_iterations;
    ICacheBuster& buster = *this_thread.icache_buster;

    for (int i = 0; i < num_iterations; i++) {
      buster.RunNextMethod();
    }
  }

  // Run feature extraction if enabled
  runFeatureExtraction(this_thread);

  // Stage 2: PageRank ranking workload (CPU-intensive, parallelized).
  // This handler is for kPageRankRequestType only — DLRM workload is
  // routed to DLRMRequestHandler in main(), so no DLRM branch here.
  int ranking_result = 0;
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

  // Start async chain. Phase 5: when --rpc_dist_path is set, fan out
  // real RPCs to mock_services in place of the synthetic sleep. When
  // unset, this still degenerates to folly::futures::sleep so the
  // legacy behavior is preserved verbatim.
  simulateIoOrFanout(
      this_thread, total_io_latency_ms, timekeeper.get(), ioThreadPool.get())
      .thenValue([ranking_result, random_string, srvIOThreadPool,
                  srv_io_threads, num_objects](folly::Unit) {
        // Stage 4: Compression and serialization
        auto compressed = compressPayload(random_string, ranking_result);
        auto per_thread_num_objects = num_objects / srv_io_threads;

        std::vector<folly::Future<int>> compressionFutures;
        for (int i = 0; i < srv_io_threads; i++) {
          auto f = folly::via(srvIOThreadPool.get(), [per_thread_num_objects]() {
            auto resp = generateResponse(per_thread_num_objects);
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
        ranking::RankingResponse resp = generateResponse(per_thread_num_objects);

        auto payloadiobufq = serializePayload(resp);
        auto buf = payloadiobufq.move();

        context_ptr->sendResponse(buf->data(), buf->length());
      })
      .thenError(folly::tag_t<std::exception>{}, [context_ptr](const std::exception& e) {
        // Error handling
        std::cerr << "Async request handler error: " << e.what() << std::endl;
        context_ptr->sendResponse(nullptr, 0);
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
    int thread_id,
    feedsim::RequestContext& context,
    std::vector<ThreadData>& thread_data) {
  auto& this_thread = thread_data[thread_id];

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
    context.sendResponse(nullptr, 0);
    return;
  }

  // Extract story content from request (Phase 3)
  std::vector<std::string> story_contents;
  if (request.story_batch().has_value()) {
    const auto& batch = request.story_batch().value();
    const auto& stories = *batch.stories();
    story_contents.reserve(stories.size());
    for (const auto& story : stories) {
      story_contents.push_back(*story.content());
    }
  }

  // Run feature extraction if enabled (with story content if available)
  runFeatureExtraction(this_thread, story_contents);

  // Populate CopyContext structData from story content when available
  if (!story_contents.empty()) {
    std::vector<float> story_dense;
    std::vector<int64_t> story_sparse;
    std::vector<float> temp_struct(512);
    populateFromStories(
        story_contents, story_dense, story_sparse,
        temp_struct.data(), static_cast<int>(temp_struct.size()));
  }

  // Choose DLRM inference path. If the client included DLRMFeatures,
  // run inferWithFeatures (no server-side RNG for inputs). Otherwise
  // fall back to server-side feature generation inside DLRM::infer.
  folly::Future<int> inference_future = folly::makeFuture<int>(0);
  if (this_thread.dlrm_ranker) {
    int num_inferences = *request.num_inferences();
    if (request.dlrm_features().has_value()) {
      const auto& features = request.dlrm_features().value();
      int batch_size = *features.batch_size();
      // Copy into shared_ptrs so they outlive the async chain.
      auto dense = std::make_shared<std::vector<float>>();
      const auto& src_dense = *features.dense_features();
      dense->reserve(src_dense.size());
      for (double v : src_dense) {
        dense->push_back(static_cast<float>(v));
      }
      auto sparse = std::make_shared<std::vector<int64_t>>(
          features.sparse_features()->begin(),
          features.sparse_features()->end());
      inference_future = dlrmInferenceClientSide(
          this_thread, std::move(dense), std::move(sparse),
          batch_size, num_inferences);
    } else {
      inference_future =
          dlrmInferenceServerSide(this_thread, num_inferences);
    }
  }

  // Move context into shared_ptr for async lifetime
  auto context_ptr =
      std::make_shared<feedsim::RequestContext>(std::move(context));

  // Capture values needed for async stages
  auto srvCPUThreadPool = this_thread.srvCPUThreadPool;
  auto srvIOThreadPool = this_thread.srvIOThreadPool;
  auto ioThreadPool = this_thread.ioThreadPool;
  auto timekeeperPool = this_thread.timekeeperPool;
  search::PointerChase* pointer_chaser = this_thread.pointer_chaser.get();

  int io_latency_ms = this_thread.getNextIOLatencyMs();
  int num_io_stages = args.io_stages_arg;
  int io_stage_latency_ms = args.io_stage_latency_ms_arg;
  int srv_io_threads = args.srv_io_threads_arg;
  int srv_threads = args.srv_threads_arg;
  int num_objects = args.num_objects_arg;
  int chase_iterations = args.chase_iterations_arg;

  int total_io_latency_ms = (num_io_stages > 1)
      ? (num_io_stages * io_stage_latency_ms)
      : io_latency_ms;

  // Pipeline: DLRM inference -> I/O sleep (or RPC fanout) -> compression
  // -> pointer chase -> generate+send response. Everything chains via
  // futures so the handler thread returns immediately and the request
  // is processed entirely off the dispatcher thread.
  //
  // Phase 5: when --rpc_dist_path is set, the I/O sleep is replaced by
  // an outbound RPC fanout to mock_services (see simulateIoOrFanout).
  auto timekeeper = timekeeperPool->getTimekeeper();
  ThreadData* this_thread_ptr = &this_thread;
  std::move(inference_future)
      .via(ioThreadPool.get())
      .thenValue([this_thread_ptr, total_io_latency_ms, timekeeper, ioThreadPool](
                     int prediction_result) {
        return simulateIoOrFanout(
                   *this_thread_ptr,
                   total_io_latency_ms,
                   timekeeper.get(),
                   ioThreadPool.get())
            .thenValue([prediction_result](folly::Unit) {
              return prediction_result;
            });
      })
      .thenValue([srvIOThreadPool, srv_io_threads, num_objects](
                     int prediction_result) {
        auto per_thread_num_objects = num_objects / srv_io_threads;
        std::vector<folly::Future<int>> compressionFutures;
        for (int i = 0; i < srv_io_threads; i++) {
          auto f = folly::via(
              srvIOThreadPool.get(), [per_thread_num_objects]() {
                auto resp = generateResponse(per_thread_num_objects);
                auto payloadiobufq = serializePayload(resp);
                auto buf = payloadiobufq.move();
                const auto compress_length =
                    buf->computeChainDataLength() / 2;
                size_t total_size = 0;
                for (auto range : *buf) {
                  if (total_size >= compress_length) break;
                  auto iobuf = folly::IOBuf::copyBuffer(
                      range.data(), range.size());
                  auto c = compressThrift(std::move(iobuf));
                  total_size += range.size();
                }
                return 1;
              });
          compressionFutures.push_back(std::move(f));
        }
        return folly::collectAll(std::move(compressionFutures))
            .via(srvIOThreadPool.get())
            .thenValue([prediction_result](
                           std::vector<folly::Try<int>> results) {
              int total = prediction_result;
              for (auto& r : results) {
                if (r.hasValue()) total += r.value();
              }
              return total;
            });
      })
      .thenValue([pointer_chaser, srvCPUThreadPool, srv_threads,
                  chase_iterations](int prev_result) {
        auto per_thread_chase_iterations = chase_iterations / srv_threads;
        std::vector<folly::Future<int>> chaseFutures;
        for (int i = 0; i < srv_threads; i++) {
          auto f = folly::via(
              srvCPUThreadPool.get(),
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
      .thenValue([context_ptr, srv_io_threads, num_objects](
                     int /*final_result*/) {
        auto per_thread_num_objects = num_objects / srv_io_threads;
        auto resp = generateResponse(per_thread_num_objects);
        folly::IOBufQueue bufq;
        apache::thrift::CompactSerializer::serialize(resp, &bufq);
        auto buf = bufq.move();
        context_ptr->sendResponse(buf->data(), buf->length());
      })
      .thenError(folly::tag_t<std::exception>{},
                 [context_ptr](const std::exception& e) {
                   std::cerr << "DLRM request handler error: " << e.what()
                             << std::endl;
                   context_ptr->sendResponse(nullptr, 0);
                 });
}
#endif // FEEDSIM_USE_DLRM

void PageRankRequestHandler(
    int thread_id,
    feedsim::RequestContext& context,
    std::vector<ThreadData>& thread_data) {
  auto& this_thread = thread_data[thread_id];
  search::PointerChase& chaser = *this_thread.pointer_chaser;

  // ICacheBuster stage (adds I-cache pressure) - PAGERANK only
  if (this_thread.icache_buster) {
    const int min_iterations = std::max(args.min_icache_iterations_arg, 0);
    const int num_iterations =
        static_cast<int>(this_thread.latency_distribution(this_thread.rng)) +
        min_iterations;
    ICacheBuster& buster = *this_thread.icache_buster;

    for (int i = 0; i < num_iterations; i++) {
      buster.RunNextMethod();
    }
  }

  // Run feature extraction if enabled
  runFeatureExtraction(this_thread);

  // PageRank ranking stage. This handler is for kPageRankRequestType only —
  // DLRM workload is routed to DLRMRequestHandler in main().
  int result = 0;
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

  // I/O simulation stage. Phase 5: when --rpc_dist_path is set, replaced
  // with an outbound RPC fanout to mock_services. .get() blocks the
  // request handler in this sync path (preserves existing behavior).
  auto timekeeper = this_thread.timekeeperPool->getTimekeeper();
  auto s = simulateIoOrFanout(
               this_thread,
               args.io_time_ms_arg,
               timekeeper.get(),
               this_thread.ioThreadPool.get())
               .thenValue([&](folly::Unit) { return result + 1; });
  result = std::move(s).get();

  auto compressed = compressPayload(this_thread.random_string, result);

  auto per_thread_num_objects = args.num_objects_arg / args.srv_io_threads_arg;

  std::vector<folly::Future<int>> compressionFutures;
  for (int i = 0; i < args.srv_io_threads_arg; i++) {
    auto f = folly::via(this_thread.srvIOThreadPool.get(), [&]() {
      auto resp = generateResponse(per_thread_num_objects);
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
  auto r = generateResponse(per_thread_num_objects);
  ranking::RankingResponse resp = r;

  // Serialize into FBThrift
  auto payloadiobufq = serializePayload(resp);
  auto buf = payloadiobufq.move();

  auto uncompressed = decompressPayload(compressed);
  auto resp1 = deserializePayload(buf.get());

  context.sendResponse(buf->data(), buf->length());
}

// ============================================================================
// Phase 4 shim handlers — exercise the new prod-shaped thrift schema and
// dispatch path. Heavy methods (getStoriesUncompressed, getAllStories) route
// to the existing DLRMRequestHandler so QPS/CPU profile is unchanged. Light
// methods (createAndPrimeSession, streamData, streamIfrPriorityRanking) send
// a small response without invoking DLRMRequestHandler — production p50
// latency and response size for these are tiny (4-44 B, 3-13 ms) and we don't
// want Phase 4 to attribute DLRM CPU to them. Phase 6 replaces these shims
// with real per-method handlers.
// ============================================================================

namespace {

// Helper: send a small fixed-size response after validating the inbound
// schema. Used by the three "light" methods. The payload bytes are filled
// with zeros — Phase 6 will populate real fields.
template <typename ResponseT>
void sendTinyResponse(feedsim::RequestContext& context, ResponseT&& response) {
  folly::IOBufQueue queue;
  apache::thrift::CompactSerializer::serialize(response, &queue);
  auto buf = queue.move();
  if (buf) {
    // CompactSerializer may chain IOBufs; coalesce so sendResponse sees the
    // full payload instead of just the head segment.
    buf->coalesce();
    context.sendResponse(buf->data(), buf->length());
  } else {
    context.sendResponse(nullptr, 0);
  }
}

} // namespace

void CreateAndPrimeSessionRequestHandler(
    int /*thread_id*/,
    feedsim::RequestContext& context,
    std::vector<ThreadData>& /*thread_data*/) {
  ranking::CreateAndPrimeSessionRequest typed_req;
  try {
    folly::IOBuf buf(
        folly::IOBuf::WRAP_BUFFER, context.payload, context.payload_length);
    apache::thrift::CompactSerializer::deserialize(&buf, typed_req);
  } catch (const std::exception& e) {
    std::cerr << "CreateAndPrimeSession: deserialize failed: " << e.what()
              << std::endl;
    context.sendResponse(nullptr, 0);
    return;
  }
  ranking::CreateAndPrimeSessionResponse resp;
  // 32-char hex placeholder; Phase 6 generates a real session_id.
  resp.session_id() = "00000000000000000000000000000000";
  resp.status_code() = 0;
  sendTinyResponse(context, resp);
}

void GetStoriesUncompressedRequestHandler(
    int thread_id,
    feedsim::RequestContext& context,
    std::vector<ThreadData>& thread_data) {
  ranking::GetStoriesRequest typed_req;
  try {
    folly::IOBuf buf(
        folly::IOBuf::WRAP_BUFFER, context.payload, context.payload_length);
    apache::thrift::CompactSerializer::deserialize(&buf, typed_req);
  } catch (const std::exception& e) {
    std::cerr << "GetStoriesUncompressed: deserialize failed: " << e.what()
              << std::endl;
    context.sendResponse(nullptr, 0);
    return;
  }
#ifdef FEEDSIM_USE_DLRM
  DLRMRequestHandler(thread_id, context, thread_data);
#else
  (void)thread_id;
  (void)thread_data;
  context.sendResponse(nullptr, 0);
#endif
}

void GetAllStoriesRequestHandler(
    int thread_id,
    feedsim::RequestContext& context,
    std::vector<ThreadData>& thread_data) {
  ranking::GetAllStoriesRequest typed_req;
  try {
    folly::IOBuf buf(
        folly::IOBuf::WRAP_BUFFER, context.payload, context.payload_length);
    apache::thrift::CompactSerializer::deserialize(&buf, typed_req);
  } catch (const std::exception& e) {
    std::cerr << "GetAllStories: deserialize failed: " << e.what() << std::endl;
    context.sendResponse(nullptr, 0);
    return;
  }
#ifdef FEEDSIM_USE_DLRM
  DLRMRequestHandler(thread_id, context, thread_data);
#else
  (void)thread_id;
  (void)thread_data;
  context.sendResponse(nullptr, 0);
#endif
}

void StreamDataRequestHandler(
    int /*thread_id*/,
    feedsim::RequestContext& context,
    std::vector<ThreadData>& /*thread_data*/) {
  ranking::StreamDataRequest typed_req;
  try {
    folly::IOBuf buf(
        folly::IOBuf::WRAP_BUFFER, context.payload, context.payload_length);
    apache::thrift::CompactSerializer::deserialize(&buf, typed_req);
  } catch (const std::exception& e) {
    std::cerr << "StreamData: deserialize failed: " << e.what() << std::endl;
    context.sendResponse(nullptr, 0);
    return;
  }
  ranking::StreamDataResponse resp;
  resp.ack_code() = 0;
  sendTinyResponse(context, resp);
}

void StreamIfrPriorityRankingRequestHandler(
    int /*thread_id*/,
    feedsim::RequestContext& context,
    std::vector<ThreadData>& /*thread_data*/) {
  ranking::StreamIfrPriorityRankingRequest typed_req;
  try {
    folly::IOBuf buf(
        folly::IOBuf::WRAP_BUFFER, context.payload, context.payload_length);
    apache::thrift::CompactSerializer::deserialize(&buf, typed_req);
  } catch (const std::exception& e) {
    std::cerr << "StreamIfrPriorityRanking: deserialize failed: " << e.what()
              << std::endl;
    context.sendResponse(nullptr, 0);
    return;
  }
  ranking::StreamIfrPriorityRankingResponse resp;
  resp.ack_code() = 0;
  sendTinyResponse(context, resp);
}

int main(int argc, char** argv) {
  if (cmdline_parser(argc, argv, &args) != 0) {
    std::cerr << "cmdline_parser failed" << std::endl;
    return 1;
  }

  // Logging level is no longer used (oldisim log macros removed).
  // Verbose/quiet flags are parsed but have no effect.

  // Determine workload type
  std::string workload_type_str = args.workload_type_arg;
  if (workload_type_str == "dlrm") {
#ifdef FEEDSIM_USE_DLRM
    g_workload_type = WorkloadType::DLRM;
    std::cout << "Using DLRM workload type" << std::endl;
#else
    std::cerr << "DLRM workload requested but FEEDSIM_USE_DLRM is not defined. "
                 "Rebuild with LibTorch support." << std::endl;
    return 1;
#endif
  } else {
    g_workload_type = WorkloadType::PAGERANK;
    std::cout << "Using PageRank workload type" << std::endl;
  }

  // Load Silesia corpus if --silesia_dir was given. Server-side response
  // generation will then pull bytes from the corpus instead of running
  // xor128() RNG (which previously consumed ~15% of CPU with no production
  // analog).
  if (args.silesia_dir_given) {
    g_silesia_loader = std::make_unique<ranking::SilesiaLoader>();
    if (!g_silesia_loader->loadDirectory(args.silesia_dir_arg)) {
      std::cerr << "Failed to load Silesia corpus from: "
                << args.silesia_dir_arg << std::endl;
      return 1;
    }
    g_silesia_response_gen =
        std::make_unique<ranking::generators::SilesiaResponseGenerator>(
            g_silesia_loader.get());
    std::cout << "Server response generator: Silesia ("
              << g_silesia_loader->numFiles() << " files, "
              << (g_silesia_loader->totalSize() / (1024 * 1024)) << " MB)"
              << std::endl;
  } else {
    std::cout << "Server response generator: xor128 RNG (no --silesia_dir)"
              << std::endl;
  }

  // Phase 5: load rpc_dist.json and instantiate the RpcDistRegistry. When
  // --rpc_dist_path is empty (default) OR --use_legacy_sleep is set, the
  // legacy folly::futures::sleep I/O simulation is used everywhere --
  // preserving the regression-safety A/B comparison path. Otherwise,
  // request handlers issue real outbound RPCs to the mock_services Thrift
  // server (must already be running on
  // --mock_services_host:--mock_services_port).
  //
  // --use_legacy_sleep wins over --rpc_dist_path so later diffs in the
  // stack can run end-to-end integration tests without the mock_services
  // side process. The g_rpc_registry stays null in that case, which makes
  // every ThreadStartup skip MockServicesClient construction (gate at
  // `rpc_registry != nullptr`) and simulateIoOrFanout fall through to
  // folly::futures::sleep (gate at `mock_client != nullptr`).
  if (args.use_legacy_sleep_flag) {
    std::cout << "RPC fanout: disabled (--use_legacy_sleep override);"
              << " using legacy folly::futures::sleep" << std::endl;
  } else if (args.rpc_dist_path_given &&
             std::string(args.rpc_dist_path_arg).size() > 0) {
    auto registry = std::make_unique<ranking::RpcDistRegistry>();
    if (!registry->load(args.rpc_dist_path_arg)) {
      std::cerr << "Failed to load rpc_dist.json from: "
                << args.rpc_dist_path_arg << std::endl;
      return 1;
    }
    g_rpc_registry = std::move(registry);
    g_rpc_silesia = g_silesia_loader.get(); // may be nullptr; that's fine
    std::cout << "RPC fanout: enabled (target "
              << args.mock_services_host_arg << ":"
              << args.mock_services_port_arg
              << ", scale=" << args.rpc_fanout_scale_arg << ")"
              << std::endl;
  } else {
    std::cout << "RPC fanout: disabled (no --rpc_dist_path); using legacy"
              << " folly::futures::sleep" << std::endl;
  }

  int fake_argc = 1;
  char* fake_argv[2] = {const_cast<char*>("./LeafNodeRank"), nullptr};
  char** sargv = static_cast<char**>(fake_argv);
  folly::init(&fake_argc, &sargv);

  // Phase 4: production-shaped thread pools. Names (visible in
  // /proc/$pid/task/*/comm and Strobelight) match the multifeed_aggregator
  // prod profile: ThriftSrv.IO, RANKER, SREventBase, GlobalCPUThread.
  const unsigned int nproc = folly::available_concurrency();

  // GlobalCPUThread: shared folly singleton. DLRM inference, feature
  // extraction, and compression all dispatch here. DO NOT construct a
  // second CPUThreadPoolExecutor named "GlobalCPUThreadPool" — folly's
  // global executor (folly/executors/GlobalExecutor.cpp) already has that
  // name and is sized to nproc by default.
  auto globalCpuKa = folly::getGlobalCPUExecutor();
  folly::Executor* globalCpuRaw = globalCpuKa.get();
  auto globalCpuKaPtr =
      std::make_shared<folly::Executor::KeepAlive<>>(std::move(globalCpuKa));
  // Aliasing shared_ptr: holds the KeepAlive alive, exposes raw Executor*.
  std::shared_ptr<folly::Executor> cpuThreadPool(globalCpuKaPtr, globalCpuRaw);

  // RANKER: ranking-orchestration / response-generation pool. Sized to
  // nproc/2 by default (matches CPL prod: 26 RANKER threads on a
  // 52-logical-core host). Replaces the legacy "srvCPUThread" pool.
  const int rankerThreads = (args.ranker_threads_arg > 0)
      ? args.ranker_threads_arg
      : std::max(1, static_cast<int>(nproc) / 2);
  auto srvCPUThreadPool = std::make_shared<folly::CPUThreadPoolExecutor>(
      rankerThreads,
      std::make_shared<folly::NamedThreadFactory>("RANKER"));

  // Legacy pool kept only for compression callsites until Phase 6.
  auto srvIOThreadPool = std::make_shared<folly::CPUThreadPoolExecutor>(
      args.srv_io_threads_arg,
      std::make_shared<folly::NamedThreadFactory>("srvIOThread"));

  // ThriftSrv.IO: inbound RPC IO loop. Renamed (was anonymous folly default).
  auto ioThreadPool = std::make_shared<folly::IOThreadPoolExecutor>(
      args.io_threads_arg,
      std::make_shared<folly::NamedThreadFactory>("ThriftSrv.IO"));

  // SREventBase: outbound-RPC EventBase pool. Idle in Phase 4 (Phase 5
  // wires mock_services fanout to it). Sized 0.7 * nproc by default
  // (matches CPL prod: 39 SREventBase threads on a 52-logical-core host).
  const int srEventBaseThreads = (args.sr_event_base_threads_arg > 0)
      ? args.sr_event_base_threads_arg
      : std::max(1, (static_cast<int>(nproc) * 7) / 10);
  auto srEventBasePool = std::make_shared<folly::IOThreadPoolExecutor>(
      srEventBaseThreads,
      std::make_shared<folly::NamedThreadFactory>("SREventBase"));

  std::cout << "Thread pools (nproc=" << nproc << "): "
            << "GlobalCPUThread (folly singleton), "
            << "RANKER=" << rankerThreads << ", "
            << "SREventBase=" << srEventBaseThreads << " (idle in Phase 4), "
            << "ThriftSrv.IO=" << args.io_threads_arg << ", "
            << "srvIOThread (legacy compression)=" << args.srv_io_threads_arg
            << std::endl;

  auto timekeeperPool =
      std::make_shared<ranking::TimekeeperPool>(args.timekeeper_threads_arg);

  // Warm up all thread pools to ensure threads are spawned and ready
  // This prevents cold-start latency spikes during actual request processing
  std::cout << "Warming up thread pools..." << std::endl;
  {
    const int warmup_tasks = 100;  // Run multiple tasks to ensure all threads are active

    // Warm up CPU thread pool (= folly global CPU executor).
    std::vector<folly::Future<int>> cpuFutures;
    for (int i = 0; i < warmup_tasks; i++) {
      cpuFutures.push_back(folly::via(cpuThreadPool.get(), []() {
        volatile int sum = 0;
        for (int j = 0; j < 1000; j++) sum += j;
        return static_cast<int>(sum);
      }));
    }
    folly::collectAll(std::move(cpuFutures)).get();

    // Warm up SREventBase pool so threads spawn and Strobelight sees them
    // even when nothing is dispatched there in Phase 4.
    std::vector<folly::Future<int>> srEbFutures;
    for (int i = 0; i < warmup_tasks; i++) {
      srEbFutures.push_back(folly::via(srEventBasePool.get(), []() {
        return 1;
      }));
    }
    folly::collectAll(std::move(srEbFutures)).get();

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
      std::cerr << "DLRM workload requires --dlrm_model_path" << std::endl;
      return 1;
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

  feedsim::FeedSimServer server(args.port_arg);
  server.setThreadStartupCallback([&](int thread_id) {
#ifdef FEEDSIM_USE_DLRM
    return ThreadStartup(
        thread_id,
        thread_data,
        params,
        cpuThreadPool,
        srvCPUThreadPool,
        srvIOThreadPool,
        ioThreadPool,
        srEventBasePool,
        timekeeperPool,
        shared_dlrm_ranker);
#else
    return ThreadStartup(
        thread_id,
        thread_data,
        params,
        cpuThreadPool,
        srvCPUThreadPool,
        srvIOThreadPool,
        ioThreadPool,
        srEventBasePool,
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

    server.registerQueryCallback(
        ranking::kPageRankRequestType,
        [&thread_data](int thread_id, feedsim::RequestContext& context) {
          return AsyncPageRankRequestHandler(thread_id, context, thread_data);
        });
  } else {
    std::cout << "Using BLOCKING I/O mode (original behavior)" << std::endl;
    server.registerQueryCallback(
        ranking::kPageRankRequestType,
        [&thread_data](int thread_id, feedsim::RequestContext& context) {
          return PageRankRequestHandler(thread_id, context, thread_data);
        });
  }

#ifdef FEEDSIM_USE_DLRM
  // Register DLRM request handler for client-side features and/or Silesia stories
  // Always register when DLRM is compiled in — the handler handles both
  // DLRM inference and story-only requests gracefully.
  std::cout << "Registering DLRM request handler (client features / stories)" << std::endl;
  server.registerQueryCallback(
      ranking::kDLRMRequestType,
      [&thread_data](int thread_id, feedsim::RequestContext& context) {
        return DLRMRequestHandler(thread_id, context, thread_data);
      });
#endif

  // Phase 4: register the 5 production-shaped inbound methods. Heavy methods
  // (getStoriesUncompressed, getAllStories) route to DLRMRequestHandler so
  // CPU profile is unchanged. Light methods (createAndPrimeSession,
  // streamData, streamIfrPriorityRanking) send a tiny response to match
  // prod p50 (4-44 B, 3-13 ms latency).
  std::cout << "Registering Phase 4 prod-shaped inbound method handlers"
            << std::endl;
  server.registerQueryCallback(
      ranking::kCreateAndPrimeSessionRequestType,
      [&thread_data](int thread_id, feedsim::RequestContext& context) {
        return CreateAndPrimeSessionRequestHandler(
            thread_id, context, thread_data);
      });
  server.registerQueryCallback(
      ranking::kGetStoriesUncompressedRequestType,
      [&thread_data](int thread_id, feedsim::RequestContext& context) {
        return GetStoriesUncompressedRequestHandler(
            thread_id, context, thread_data);
      });
  server.registerQueryCallback(
      ranking::kGetAllStoriesRequestType,
      [&thread_data](int thread_id, feedsim::RequestContext& context) {
        return GetAllStoriesRequestHandler(thread_id, context, thread_data);
      });
  server.registerQueryCallback(
      ranking::kStreamDataRequestType,
      [&thread_data](int thread_id, feedsim::RequestContext& context) {
        return StreamDataRequestHandler(thread_id, context, thread_data);
      });
  server.registerQueryCallback(
      ranking::kStreamIfrPriorityRankingRequestType,
      [&thread_data](int thread_id, feedsim::RequestContext& context) {
        return StreamIfrPriorityRankingRequestHandler(
            thread_id, context, thread_data);
      });

  server.setNumThreads(args.threads_arg);
  server.setThreadPinning(args.noaffinity_given == 0u);
  server.setThreadLoadBalancing(args.noloadbalance_given == 0u);

  server.enableMonitoring(args.monitor_port_arg);

  server.run();

  return 0;
}

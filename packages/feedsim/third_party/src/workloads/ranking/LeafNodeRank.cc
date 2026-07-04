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
#include <cstdio>
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
#include <folly/container/F14Map.h>
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
#include <folly/io/async/EventBase.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/system/HardwareConcurrency.h>

#include <thrift/lib/cpp2/protocol/CompactProtocol.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include "FeedSimServer.h"
#include "FeedSimProtocol.h"
#include "LatencyHistogram.h"

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

// Phase 6: per-session bookkeeping kept in a per-RANKER-thread sharded
// map keyed by query_id. The driver mints query_id = (thread_id << 32) |
// session_counter so all 4-6 inbound RPCs for one session naturally
// land on the same shard via session_id -> thread_id derivation, which
// keeps the lookup lock-free.
//
// Fields are deliberately lightweight; we record just enough to
// represent prod's "session has started, here is what was streamed"
// state that downstream getStoriesUncompressed / getAllStories /
// streamData* observe. We do NOT actually consume the streamed payloads
// — they are stashed only so we pay the memory cost like prod.
struct SessionState {
  int64_t query_id = 0;
  int64_t user_id = 0;
  uint64_t created_at_ns = 0;
  std::string session_id;
  std::string mobile_app_version;
  // Stash decompressed streamData payloads (we don't use them, but we
  // pay the memory cost like prod).
  std::vector<std::string> stream_payloads;
  // Stash IFR objects from streamIfrPriorityRanking.
  std::vector<std::string> ifr_payloads;
};

struct ThreadData {
  // Phase 4 thread-pool aliases (names kept for callsite stability):
  //   cpuThreadPool       -> folly::getGlobalCPUExecutor() ("GlobalCPUThread")
  //   srvCPUThreadPool    -> RANKER pool (NamedThreadFactory("RANKER"))
  //   ioThreadPool        -> ThriftSrv.IO pool (NamedThreadFactory("ThriftSrv.IO"))
  //   srEventBasePool     -> outbound-RPC EventBase pool (carries fanout
  //                          to mock_services). Replaces the legacy
  //                          "srvIOThread" pool which used to host
  //                          throw-away datagen + compression as a
  //                          placeholder for real outbound work.
  std::shared_ptr<folly::Executor> cpuThreadPool;
  std::shared_ptr<folly::CPUThreadPoolExecutor> srvCPUThreadPool;
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

  // Phase 6: per-thread session map. Sharded by query_id high bits
  // (driver puts thread_id in the high 32 bits of query_id), so the
  // 4-6 inbound RPCs for one session naturally land on the same
  // dispatcher (ThriftSrv.IO worker) thread. All reads/writes of
  // `sessions` and `session_id_to_query_id` MUST happen on the
  // dispatcher thread that owns this ThreadData — async work on
  // RANKER / GlobalCPUThread / SREventBase must hop back to the
  // dispatcher (e.g. via folly::via(this_thread.dispatcher_evb)) before
  // touching either map.
  folly::F14FastMap<int64_t, SessionState> sessions;
  // Phase 6 (Issue 5 fix): side-table from session_id (the on-wire
  // identifier carried by streamIfrPriorityRanking) back to the
  // query_id that keys `sessions`. Built when CreateAndPrime
  // registers a session.
  folly::F14FastMap<std::string, int64_t> session_id_to_query_id;
  uint64_t session_counter = 0;
  // Phase 6 (Issue 4 fix): the dispatcher EventBase that owns this
  // ThreadData. Captured on the first handler call (lazy: the worker
  // EventBase isn't known at ThreadStartup time). Used to hop async
  // continuations back onto the dispatcher before mutating `sessions`
  // / `session_id_to_query_id`.
  folly::EventBase* dispatcher_evb = nullptr;

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
    const std::shared_ptr<folly::IOThreadPoolExecutor>& ioThreadPool,
    const std::shared_ptr<folly::IOThreadPoolExecutor>& srEventBasePool,
    const std::shared_ptr<ranking::TimekeeperPool>& timekeeperPool,
    const std::shared_ptr<ranking::dwarfs::DLRM>& shared_dlrm_ranker) {
  auto& this_thread = thread_data[thread_id];
  this_thread.cpuThreadPool = cpuThreadPool;
  this_thread.srvCPUThreadPool = srvCPUThreadPool;
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
    const std::shared_ptr<folly::IOThreadPoolExecutor>& ioThreadPool,
    const std::shared_ptr<folly::IOThreadPoolExecutor>& srEventBasePool,
    const std::shared_ptr<ranking::TimekeeperPool>& timekeeperPool) {
  auto& this_thread = thread_data[thread_id];
  auto graph = params.makeGraphCopy(g_shared_graph);
  this_thread.cpuThreadPool = cpuThreadPool;
  this_thread.srvCPUThreadPool = srvCPUThreadPool;
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
// Debug histograms for ad-hoc instrumentation of mock_services fanout
// (issueOutboundFanout total + per-dispatch round trip + sampled
// latency_us). Periodically dumped to stderr from a background thread
// in main().
static feedsim::LatencyHistogram g_fanout_total_us;
static feedsim::LatencyHistogram g_dispatch_us;
static feedsim::LatencyHistogram g_sampled_lat_us;

static folly::Future<int> issueOutboundFanout(
    ThreadData& td, double scale) {
  if (td.mock_client == nullptr || td.rpc_registry == nullptr) {
    // Defensive: caller should have checked --rpc_dist_path.
    return folly::makeFuture<int>(0);
  }

  uint64_t fanout_start_us = feedsim::nowUs();

  std::vector<folly::Future<int>> futs;
  futs.reserve(128);

  for (size_t i = 0; i < ranking::kNumMethods; ++i) {
    auto m = static_cast<ranking::MethodIdx>(i);
    // Round to the nearest integer call count and skip methods that don't
    // round up to at least 1. The previous std::max(1, ...) floor inflated
    // the share of low-weighted but slow methods (e.g. tail-latency outliers
    // that production hits ~once per 200 sessions) to once-per-session at
    // small --rpc_fanout_scale, which distorted both the per-method ratios
    // and the aggregate latency distribution toward the slow tail.
    int n = static_cast<int>(
        std::round(ranking::perSessionCounts()[i] * scale));
    if (n == 0) {
      continue;
    }

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
      g_sampled_lat_us.record(static_cast<uint64_t>(std::max(0, lat_us)));

      uint64_t dispatch_start_us = feedsim::nowUs();
      futs.push_back(td.mock_client
                         ->dispatchByEnum(m, req, lat_us)
                         .via(td.srEventBasePool.get())
                         .thenValue([dispatch_start_us](std::string&&) {
                           g_dispatch_us.record(
                               feedsim::nowUs() - dispatch_start_us);
                           return 1;
                         })
                         .thenError(
                             folly::tag_t<std::exception>{},
                             [dispatch_start_us](const std::exception&) {
                               g_dispatch_us.record(
                                   feedsim::nowUs() - dispatch_start_us);
                               return 0;
                             }));
    }
  }

  return folly::collectAll(std::move(futs))
      .via(td.srEventBasePool.get())
      .thenValue([fanout_start_us](std::vector<folly::Try<int>> results) {
        g_fanout_total_us.record(feedsim::nowUs() - fanout_start_us);
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
  auto srvCPUThreadPool = this_thread.srvCPUThreadPool;
  auto ioThreadPool = this_thread.ioThreadPool;
  auto timekeeperPool = this_thread.timekeeperPool;
  search::PointerChase* pointer_chaser = this_thread.pointer_chaser.get();

  // Get I/O latency for this request (configurable distribution)
  int io_latency_ms = this_thread.getNextIOLatencyMs();

  // For multi-stage I/O simulation
  int num_io_stages = args.io_stages_arg;
  int io_stage_latency_ms = args.io_stage_latency_ms_arg;

  // Capture values for lambda captures
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
      .thenValue([pointer_chaser, srvCPUThreadPool, srv_threads,
                  chase_iterations, ranking_result](folly::Unit) {
        // Stage 4: Pointer chase. The legacy throw-away
        // generateResponse + serializePayload + compressThrift fanout
        // on srvIOThreadPool that used to sit between Stage 3 and this
        // stage was a placeholder for outbound RPC work; that work is
        // now paid for by issueOutboundFanout (see Stage 3 /
        // simulateIoOrFanout) on srEventBasePool.
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
            .thenValue([ranking_result](std::vector<folly::Try<int>> results) {
              int total = ranking_result;
              for (auto& r : results) {
                if (r.hasValue()) total += r.value();
              }
              return total;
            });
      })
      .thenValue([context_ptr, num_objects](int /*final_result*/) {
        // Stage 5: Generate and send response.
        ranking::RankingResponse resp = generateResponse(num_objects);

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
  auto ioThreadPool = this_thread.ioThreadPool;
  auto timekeeperPool = this_thread.timekeeperPool;
  search::PointerChase* pointer_chaser = this_thread.pointer_chaser.get();

  int io_latency_ms = this_thread.getNextIOLatencyMs();
  int num_io_stages = args.io_stages_arg;
  int io_stage_latency_ms = args.io_stage_latency_ms_arg;
  int srv_threads = args.srv_threads_arg;
  int num_objects = args.num_objects_arg;
  int chase_iterations = args.chase_iterations_arg;

  int total_io_latency_ms = (num_io_stages > 1)
      ? (num_io_stages * io_stage_latency_ms)
      : io_latency_ms;

  // Pipeline: DLRM inference -> I/O sleep (or RPC fanout) -> pointer
  // chase -> generate+send response. Everything chains via futures so
  // the handler thread returns immediately and the request is processed
  // entirely off the dispatcher thread.
  //
  // Phase 5: when --rpc_dist_path is set, the I/O sleep is replaced by
  // an outbound RPC fanout to mock_services on srEventBasePool (see
  // simulateIoOrFanout). The legacy throw-away
  // generateResponse + serializePayload + compressThrift fanout on
  // srvIOThreadPool that used to sit between simulateIoOrFanout and the
  // pointer chase was a placeholder for that real outbound work and is
  // now redundant.
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
      .thenValue([context_ptr, num_objects](int /*final_result*/) {
        auto resp = generateResponse(num_objects);
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

  // The legacy throw-away generateResponse + serializePayload +
  // compressThrift fanout on srvIOThreadPool that used to sit here was a
  // placeholder for outbound RPC work; that work is now paid for by
  // simulateIoOrFanout above (issueOutboundFanout on srEventBasePool
  // when --rpc_dist_path is set).

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
  auto r = generateResponse(args.num_objects_arg);
  ranking::RankingResponse resp = r;

  // Serialize into FBThrift
  auto payloadiobufq = serializePayload(resp);
  auto buf = payloadiobufq.move();

  auto resp1 = deserializePayload(buf.get());

  context.sendResponse(buf->data(), buf->length());
}

// ============================================================================
// Phase 6: real per-method handlers, replacing the Phase 4 shims.
//
// Each handler implements the per-method pipeline from
// ~/feedsim_v2/docs/phase6_researcher_notes.md §4:
//   - Deserialize on ThriftSrv.IO (the dispatcher thread).
//   - Look up / mutate the per-thread session map (sharded by query_id).
//   - For heavy methods, folly::via(rankerPool) to orchestrate, then
//     fan out runFeatureExtraction / DLRM inference / outbound RPC
//     fanout (issueOutboundFanout) on GlobalCPUThread / SREventBase.
//   - Compress and send response from the final continuation.
//
// Heavy handlers (getStoriesUncompressed, getAllStories,
// streamIfrPriorityRanking) are async — they move the request context
// into a shared_ptr and return immediately. Light handlers
// (createAndPrimeSession, streamData) stay synchronous on the
// dispatcher thread.
//
// The legacy DLRMRequestHandler / PageRankRequestHandler /
// AsyncPageRankRequestHandler are kept for now and deleted by
// Programmer-C in Phase 6-C cleanup.
// ============================================================================

namespace {

// Helper: serialize, coalesce and send a Thrift response. Used by every
// handler — both the small ack responses and the large compressed
// payload responses go through here so they share the coalesce path.
template <typename ResponseT>
void sendThriftResponse(
    feedsim::RequestContext& context, const ResponseT& response) {
  folly::IOBufQueue queue;
  apache::thrift::CompactSerializer::serialize(response, &queue);
  auto buf = queue.move();
  if (buf) {
    buf->coalesce();
    context.sendResponse(buf->data(), buf->length());
  } else {
    context.sendResponse(nullptr, 0);
  }
}

// Phase 6: synthesize a 32-char hex session_id from query_id + the
// thread's RNG. The driver uses session_id as an opaque token returned
// from createAndPrimeSession; getStories* threads it back through.
inline std::string makeSessionId(int64_t query_id, std::mt19937& rng) {
  uint64_t lo = static_cast<uint64_t>(query_id);
  uint64_t hi = (static_cast<uint64_t>(rng()) << 32) | static_cast<uint64_t>(rng());
  char buf[33];
  std::snprintf(
      buf,
      sizeof(buf),
      "%016llx%016llx",
      static_cast<unsigned long long>(hi),
      static_cast<unsigned long long>(lo));
  return std::string(buf, 32);
}

// Phase 6: extract a target response size from rpc_dist.json's inbound
// section if loaded, otherwise return the prod p50 fallback.
inline size_t inboundResponseSizeOrDefault(
    ThreadData& td, ranking::InboundIdx idx, size_t fallback) {
  if (td.rpc_registry == nullptr) return fallback;
  const auto& sampler = td.rpc_registry->inboundResponseSize(idx);
  if (!sampler.isLoaded()) return fallback;
  return static_cast<size_t>(sampler.sample(td.rpc_rng));
}

// Phase 6: build a GetStoriesResponse with `num_stories` story_infos
// padded so the serialized size hits `target_bytes`. The dominant
// space cost is the RankedStoryInfo.story_payload binary. We size each
// story_payload uniformly so total ≈ target. Run on RANKER (CPU work).
ranking::GetStoriesResponse generateGetStoriesResponse(
    int64_t query_id,
    int num_stories,
    size_t target_bytes,
    ranking::SilesiaLoader* silesia,
    std::mt19937& rng) {
  ranking::GetStoriesResponse resp;
  resp.query_id() = query_id;
  resp.status_code() = 0;
  ranking::GetStoriesResponseStats stats;
  stats.num_actions_received() = 100;
  stats.num_friends_queried() = 25;
  stats.num_object_summaries_received() = 50;
  resp.stats() = stats;

  // Reserve and populate stories. Per-story fixed overhead (CompactProtocol
  // field tags + small ints + doubles) is ~80 bytes; story_payload carries
  // the bulk. Allocate ~80B fixed overhead per story plus per-story payload.
  auto& stories = *resp.story_infos();
  stories.reserve(static_cast<size_t>(num_stories));
  size_t fixed_overhead = 80 * static_cast<size_t>(num_stories);
  size_t payload_budget = target_bytes > fixed_overhead
      ? target_bytes - fixed_overhead
      : 0;
  size_t per_story_payload =
      num_stories > 0 ? payload_budget / static_cast<size_t>(num_stories) : 0;

  for (int i = 0; i < num_stories; ++i) {
    ranking::RankedStoryInfo info;
    info.story_key() = static_cast<int64_t>(rng()) ^ query_id;
    info.actor_id() = static_cast<int64_t>(rng());
    info.target_id() = static_cast<int64_t>(rng());
    info.object_id() = static_cast<int64_t>(rng());
    info.source_type() = static_cast<int32_t>(i % 8);
    info.story_type() = static_cast<int32_t>(i % 26);
    info.time_published() = static_cast<int32_t>(rng());
    info.weight() = 0.5;
    info.weight_user() = 0.4;
    info.weight_participants() = 0.3;
    info.weight_event() = 0.2;
    info.discounted_weight() = 0.1;
    if (per_story_payload > 0) {
      std::string& payload = info.story_payload().value();
      payload.resize(per_story_payload);
      if (silesia != nullptr && silesia->isLoaded()) {
        const uint8_t* snippet = nullptr;
        size_t snippet_size = 0;
        std::string filename;
        silesia->getRandomSnippet(
            rng,
            /*min_size=*/1,
            per_story_payload,
            snippet,
            snippet_size,
            filename);
        size_t copy_size = std::min(per_story_payload, snippet_size);
        if (copy_size > 0) {
          std::memcpy(payload.data(), snippet, copy_size);
        }
        if (copy_size < per_story_payload) {
          std::memset(
              payload.data() + copy_size, 0, per_story_payload - copy_size);
        }
      } else {
        std::memset(payload.data(), 0, per_story_payload);
      }
    }
    stories.push_back(std::move(info));
  }
  return resp;
}

// Same shape as generateGetStoriesResponse but emits a
// GetAllStoriesResponse. The two responses share RankedStoryInfo so we
// could templatize, but doing so plays poorly with the typed
// `all_story_infos()` vs `story_infos()` accessors — keep two simple
// functions, mirror the body.
ranking::GetAllStoriesResponse generateGetAllStoriesResponse(
    int64_t query_id,
    int num_stories,
    size_t target_bytes,
    ranking::SilesiaLoader* silesia,
    std::mt19937& rng) {
  ranking::GetAllStoriesResponse resp;
  resp.query_id() = query_id;
  resp.status_code() = 0;
  ranking::GetStoriesResponseStats stats;
  stats.num_actions_received() = 500;
  stats.num_friends_queried() = 60;
  stats.num_object_summaries_received() = 200;
  resp.stats() = stats;

  auto& stories = *resp.all_story_infos();
  stories.reserve(static_cast<size_t>(num_stories));
  size_t fixed_overhead = 80 * static_cast<size_t>(num_stories);
  size_t payload_budget = target_bytes > fixed_overhead
      ? target_bytes - fixed_overhead
      : 0;
  size_t per_story_payload =
      num_stories > 0 ? payload_budget / static_cast<size_t>(num_stories) : 0;

  for (int i = 0; i < num_stories; ++i) {
    ranking::RankedStoryInfo info;
    info.story_key() = static_cast<int64_t>(rng()) ^ query_id;
    info.actor_id() = static_cast<int64_t>(rng());
    info.target_id() = static_cast<int64_t>(rng());
    info.object_id() = static_cast<int64_t>(rng());
    info.source_type() = static_cast<int32_t>(i % 8);
    info.story_type() = static_cast<int32_t>(i % 26);
    info.time_published() = static_cast<int32_t>(rng());
    info.weight() = 0.5;
    info.weight_user() = 0.4;
    info.weight_participants() = 0.3;
    info.weight_event() = 0.2;
    info.discounted_weight() = 0.1;
    if (per_story_payload > 0) {
      std::string& payload = info.story_payload().value();
      payload.resize(per_story_payload);
      if (silesia != nullptr && silesia->isLoaded()) {
        const uint8_t* snippet = nullptr;
        size_t snippet_size = 0;
        std::string filename;
        silesia->getRandomSnippet(
            rng,
            /*min_size=*/1,
            per_story_payload,
            snippet,
            snippet_size,
            filename);
        size_t copy_size = std::min(per_story_payload, snippet_size);
        if (copy_size > 0) {
          std::memcpy(payload.data(), snippet, copy_size);
        }
        if (copy_size < per_story_payload) {
          std::memset(
              payload.data() + copy_size, 0, per_story_payload - copy_size);
        }
      } else {
        std::memset(payload.data(), 0, per_story_payload);
      }
    }
    stories.push_back(std::move(info));
  }
  return resp;
}

// Phase 6: serialize a typed response and run the ZSTD compress step
// over the serialized bytes. Returns the (compressed) IOBuf chain
// alongside a record of original/compressed sizes for logging. Run on
// GlobalCPUThread per the section 4 thread-pool diagram.
template <typename ResponseT>
std::unique_ptr<folly::IOBuf> serializeAndCompress(const ResponseT& resp) {
  folly::IOBufQueue queue;
  apache::thrift::CompactSerializer::serialize(resp, &queue);
  auto buf = queue.move();
  if (!buf) return nullptr;
  return compressThrift(std::move(buf));
}

} // namespace

// ----------------------------------------------------------------------------
// Handler 1: createAndPrimeSession
//
// Stages: deserialize -> insert into per-thread sessions map -> serialize
// 44 B response -> sendResponse. Synchronous on ThriftSrv.IO. No DLRM,
// no fanout, no compression. Prod p50 ≈ 3 ms is naturally produced by
// deserialize + map insert; we add no artificial sleep.
// ----------------------------------------------------------------------------
void CreateAndPrimeSessionRequestHandler(
    int thread_id,
    feedsim::RequestContext& context,
    std::vector<ThreadData>& thread_data) {
  auto& this_thread = thread_data[thread_id];

  // Phase 6 (Issue 4 fix): lazy-capture the dispatcher EventBase the
  // first time we run on this worker. All subsequent async work that
  // wants to mutate `sessions` / `session_id_to_query_id` hops back
  // here.
  if (this_thread.dispatcher_evb == nullptr) {
    this_thread.dispatcher_evb =
        folly::EventBaseManager::get()->getEventBase();
  }

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

  // Build the session and insert into this thread's session map. The
  // driver mints query_id with thread_id in the high 32 bits, so the
  // 4-6 inbound calls for one session land on this same shard.
  SessionState state;
  state.query_id = *typed_req.query_id();
  state.user_id = *typed_req.user_id();
  state.created_at_ns = feedsim::getTimeNano();
  state.session_id = makeSessionId(state.query_id, this_thread.rpc_rng);

  // Phase 6 (Issue 5 fix): record session_id -> query_id so the
  // streamIfrPriorityRanking handler (which only carries session_id on
  // the wire) can resolve back to the keyed `sessions` entry instead
  // of falling back to `sessions.begin()` on an arbitrary entry.
  this_thread.session_id_to_query_id[state.session_id] = state.query_id;

  // Insert (or replace) — the driver uses a fresh session_counter per
  // session so collisions are not expected.
  this_thread.sessions[state.query_id] = std::move(state);
  ++this_thread.session_counter;

  // Build response. The session_id is 32 chars + small ints; with
  // CompactProtocol overhead this serializes to ~44 B which matches
  // the prod p50 from rpc_dist.json without padding.
  ranking::CreateAndPrimeSessionResponse resp;
  resp.session_id() = this_thread.sessions[*typed_req.query_id()].session_id;
  resp.status_code() = 0;
  sendThriftResponse(context, resp);
}

// ----------------------------------------------------------------------------
// Handler 2: getStoriesUncompressed (the heavy hitter)
//
// Stages (all from researcher §4 row 2):
//   1. Deserialize on ThriftSrv.IO (req=2.1 MB at p50, real CPU).
//   2. Look up session by query_id (shard already correct since the
//      driver routes by thread_id).
//   3. folly::via(rankerPool) to orchestrate.
//   4. In parallel on GlobalCPUThread / SREventBase:
//        (a) runFeatureExtraction(this_thread, story_contents)
//        (b) DLRM inference (client-side or server-side path)
//        (c) issueOutboundFanout(this_thread, scale=full) (~94 RPCs)
//   5. Generate response (~171 KB) on RANKER, then compress on
//      GlobalCPUThread, then sendResponse on ThriftSrv.IO.
// ----------------------------------------------------------------------------
void GetStoriesUncompressedRequestHandler(
    int thread_id,
    feedsim::RequestContext& context,
    std::vector<ThreadData>& thread_data) {
  auto& this_thread = thread_data[thread_id];

  // Phase 6 (Issue 4 fix): lazy-capture dispatcher EventBase so async
  // continuations can hop back here before mutating `sessions`.
  if (this_thread.dispatcher_evb == nullptr) {
    this_thread.dispatcher_evb =
        folly::EventBaseManager::get()->getEventBase();
  }

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

  int64_t query_id = *typed_req.query_id();

  // Story contents — Phase 7 will let the driver send them; for now the
  // request schema doesn't carry stories so we pass empty (extractors
  // fall back to RNG-derived feature inputs).
  std::vector<std::string> story_contents;

  // Snapshot the session's mobile_app_version into state — the
  // getAllStories second pass reads it from the session.
  {
    const std::string& mobile_app_version = *typed_req.mobile_app_version();
    if (!mobile_app_version.empty()) {
      auto it = this_thread.sessions.find(query_id);
      if (it != this_thread.sessions.end()) {
        it->second.mobile_app_version = mobile_app_version;
      }
    }
  }

  // Move context into shared_ptr for async lifetime.
  auto context_ptr =
      std::make_shared<feedsim::RequestContext>(std::move(context));

  // Capture pool handles so the chain can run after this_thread goes
  // out of scope (continuations land back on the captured executors).
  auto rankerPool = this_thread.srvCPUThreadPool;
  auto globalCpu = this_thread.cpuThreadPool;
  ThreadData* this_thread_ptr = &this_thread;
  int num_stories_per_response = std::max(1, args.num_stories_arg);
  size_t target_resp_size = inboundResponseSizeOrDefault(
      this_thread,
      ranking::InboundIdx::kGetStoriesUncompressed,
      /*fallback=*/171393); // prod p50

  // Pull DLRM dispatch onto the orchestrator. Detect client-side vs
  // server-side features path the same way DLRMRequestHandler does;
  // legacy GetStoriesRequest does not carry DLRMFeatures so this path
  // is server-side only at the moment, but we keep the dispatch shape
  // ready for Phase 7.
  folly::Future<int> inference_future = folly::makeFuture<int>(0);
#ifdef FEEDSIM_USE_DLRM
  if (this_thread.dlrm_ranker) {
    int num_inferences = std::max(1, args.dlrm_inferences_per_request_arg);
    inference_future =
        dlrmInferenceServerSide(this_thread, num_inferences);
  }
#endif

  // Hop onto RANKER for orchestration. From there fan out the three
  // CPU/IO stages and collectAll, then build+compress+send response.
  folly::via(rankerPool.get(), [this_thread_ptr, story_contents]() {
    // Stage 4(a): runFeatureExtraction is dispatched onto
    // GlobalCPUThread inside the lambda. Spawn a future that resolves
    // when extraction completes so the collectAll downstream sees a
    // uniform Future<Unit>.
    return folly::via(
        this_thread_ptr->cpuThreadPool.get(),
        [this_thread_ptr, story_contents]() {
          runFeatureExtraction(*this_thread_ptr, story_contents);
          return folly::unit;
        });
  })
      .thenValue([this_thread_ptr,
                  inference_future = std::move(inference_future),
                  rankerPool](folly::Unit) mutable {
        // Stage 4(b/c): DLRM inference + outbound RPC fanout, in parallel.
        // Inference future was already started above; fanout starts here.
        // Both resolve before we move on to response generation.
        auto fanout_future =
            issueOutboundFanout(*this_thread_ptr, args.rpc_fanout_scale_arg);
        std::vector<folly::Future<folly::Unit>> waits;
        waits.push_back(
            std::move(inference_future)
                .via(rankerPool.get())
                .thenValue([](int) { return folly::unit; }));
        waits.push_back(
            std::move(fanout_future)
                .via(rankerPool.get())
                .thenValue([](int) { return folly::unit; }));
        return folly::collectAll(std::move(waits))
            .via(rankerPool.get())
            .thenValue([](auto&&) { return folly::unit; });
      })
      .thenValue([this_thread_ptr,
                  query_id,
                  num_stories_per_response,
                  target_resp_size,
                  globalCpu](folly::Unit) {
        // Stage 5: response generation + compression. Generation runs on
        // RANKER (CPU work) and compression hops to GlobalCPUThread to
        // match the prod attribution (compression is in the GlobalCPU
        // category per Phase 4 §2 callsite #14).
        auto resp = generateGetStoriesResponse(
            query_id,
            num_stories_per_response,
            target_resp_size,
            this_thread_ptr->rpc_silesia,
            this_thread_ptr->rpc_rng);
        // Move resp into a shared_ptr so the next continuation owns it.
        auto resp_ptr =
            std::make_shared<ranking::GetStoriesResponse>(std::move(resp));
        return folly::via(
            globalCpu.get(),
            [resp_ptr]() {
              auto compressed = serializeAndCompress(*resp_ptr);
              // We only count the cost — the actual wire payload is the
              // uncompressed serialized form, matching the
              // "uncompressed" name of this method. compressed bytes
              // are discarded after the work is paid for.
              (void)compressed;
              folly::IOBufQueue queue;
              apache::thrift::CompactSerializer::serialize(*resp_ptr, &queue);
              auto buf = queue.move();
              if (buf) buf->coalesce();
              return buf;
            });
      })
      .thenValue([context_ptr](std::unique_ptr<folly::IOBuf> buf) {
        if (buf) {
          context_ptr->sendResponse(buf->data(), buf->length());
        } else {
          context_ptr->sendResponse(nullptr, 0);
        }
      })
      .thenError(
          folly::tag_t<std::exception>{},
          [context_ptr](const std::exception& e) {
            std::cerr << "GetStoriesUncompressed handler error: " << e.what()
                      << std::endl;
            context_ptr->sendResponse(nullptr, 0);
          });
}

// ----------------------------------------------------------------------------
// Handler 3: getAllStories (second-pass aggregation)
//
// Stages: deserialize tiny req on ThriftSrv.IO -> session lookup ->
// folly::via(rankerPool) -> issueOutboundFanout(scale * 0.5) (no DLRM,
// no extractors per researcher §4 row 3) -> generate ~1.47 MB response
// -> compress -> sendResponse.
// ----------------------------------------------------------------------------
void GetAllStoriesRequestHandler(
    int thread_id,
    feedsim::RequestContext& context,
    std::vector<ThreadData>& thread_data) {
  auto& this_thread = thread_data[thread_id];

  // Phase 6 (Issue 4 fix): lazy-capture dispatcher EventBase.
  if (this_thread.dispatcher_evb == nullptr) {
    this_thread.dispatcher_evb =
        folly::EventBaseManager::get()->getEventBase();
  }

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

  int64_t query_id = *typed_req.query_id();

  auto context_ptr =
      std::make_shared<feedsim::RequestContext>(std::move(context));

  auto rankerPool = this_thread.srvCPUThreadPool;
  auto globalCpu = this_thread.cpuThreadPool;
  ThreadData* this_thread_ptr = &this_thread;
  size_t target_resp_size = inboundResponseSizeOrDefault(
      this_thread,
      ranking::InboundIdx::kGetAllStories,
      /*fallback=*/1474443); // prod p50 ~ 1.47 MB

  // Roughly 8.6x the story count of getStoriesUncompressed to reach
  // 1.47 MB at the same per-story budget. Floor at 100 so very small
  // configs still produce a non-trivial response.
  int num_stories = std::max(args.num_stories_arg * 9, 100);

  folly::via(rankerPool.get(), [this_thread_ptr]() {
        // Stage 4: half-scale outbound fanout. No DLRM, no extractors —
        // those are paid for by getStoriesUncompressed earlier in the
        // session per researcher §4 row 3.
        return issueOutboundFanout(
                   *this_thread_ptr, args.rpc_fanout_scale_arg * 0.5)
            .thenValue([](int) { return folly::unit; });
      })
      .thenValue([this_thread_ptr,
                  query_id,
                  num_stories,
                  target_resp_size,
                  globalCpu](folly::Unit) {
        auto resp = generateGetAllStoriesResponse(
            query_id,
            num_stories,
            target_resp_size,
            this_thread_ptr->rpc_silesia,
            this_thread_ptr->rpc_rng);
        auto resp_ptr =
            std::make_shared<ranking::GetAllStoriesResponse>(std::move(resp));
        return folly::via(
            globalCpu.get(),
            [resp_ptr]() {
              auto compressed = serializeAndCompress(*resp_ptr);
              (void)compressed;
              folly::IOBufQueue queue;
              apache::thrift::CompactSerializer::serialize(*resp_ptr, &queue);
              auto buf = queue.move();
              if (buf) buf->coalesce();
              return buf;
            });
      })
      .thenValue([context_ptr](std::unique_ptr<folly::IOBuf> buf) {
        if (buf) {
          context_ptr->sendResponse(buf->data(), buf->length());
        } else {
          context_ptr->sendResponse(nullptr, 0);
        }
      })
      .thenError(
          folly::tag_t<std::exception>{},
          [context_ptr](const std::exception& e) {
            std::cerr << "GetAllStories handler error: " << e.what()
                      << std::endl;
            context_ptr->sendResponse(nullptr, 0);
          });
}

// ----------------------------------------------------------------------------
// Handler 4: streamData (fast ack)
//
// Stages: deserialize on ThriftSrv.IO -> decompress payload (cost is
// almost all the per-call latency; req can be 58 KB to 3.2 MB
// bimodal) -> stash decompressed bytes into the session prediction
// cache -> send 4-byte ack.
//
// Synchronous on the dispatcher thread per researcher §4 row 4 — the
// expected p50 is ~7 ms, dominated by deserialize + decompress, which
// are CPU-bound and not worth the via overhead at this latency.
// ----------------------------------------------------------------------------
void StreamDataRequestHandler(
    int thread_id,
    feedsim::RequestContext& context,
    std::vector<ThreadData>& thread_data) {
  auto& this_thread = thread_data[thread_id];

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

  // Decompress the streamed payload. We tolerate decompress failures
  // gracefully — production sometimes ships uncompressed payloads
  // through this path and we don't want a transient mismatch to fail
  // the whole session.
  std::string decompressed;
  {
    const std::string& blob = *typed_req.serialized_payload();
    if (!blob.empty()) {
      try {
        decompressed = decompressPayload(blob);
      } catch (const std::exception&) {
        // Fall back to using the raw bytes — the size cost is paid
        // for either way.
        decompressed = blob;
      }
    }
  }

  // Stash decompressed bytes into the per-session prediction cache.
  // We keep at most a few payloads to bound memory growth across
  // long-running sessions; the prod equivalent caps too.
  int64_t query_id = *typed_req.query_id();
  auto it = this_thread.sessions.find(query_id);
  if (it != this_thread.sessions.end()) {
    constexpr size_t kMaxStreamPayloads = 8;
    if (it->second.stream_payloads.size() >= kMaxStreamPayloads) {
      it->second.stream_payloads.erase(it->second.stream_payloads.begin());
    }
    it->second.stream_payloads.push_back(std::move(decompressed));
  }

  ranking::StreamDataResponse resp;
  resp.ack_code() = 0;
  sendThriftResponse(context, resp);
}

// ----------------------------------------------------------------------------
// Handler 5: streamIfrPriorityRanking
//
// Stages: deserialize on ThriftSrv.IO -> folly::via(rankerPool) ->
// decompress IFR objects on GlobalCPUThread -> small DLRM scoring pass
// (single batch) on GlobalCPUThread -> small fanout (scale * 0.1)
// on SREventBase -> stash on RANKER -> send 4 B ack.
//
// Async because of the fanout. Prod p50 ≈ 13 ms (req=949 KB, real CPU).
// ----------------------------------------------------------------------------
void StreamIfrPriorityRankingRequestHandler(
    int thread_id,
    feedsim::RequestContext& context,
    std::vector<ThreadData>& thread_data) {
  auto& this_thread = thread_data[thread_id];

  // Phase 6 (Issue 4 fix): lazy-capture dispatcher EventBase. The
  // continuation that mutates `sessions` MUST hop back here.
  if (this_thread.dispatcher_evb == nullptr) {
    this_thread.dispatcher_evb =
        folly::EventBaseManager::get()->getEventBase();
  }

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

  // Phase 6 (Issue 5 fix): the wire schema carries session_id, not
  // query_id. Resolve session_id -> query_id via the side-table built
  // by CreateAndPrime. If we can't resolve (e.g. IFR for a session
  // CreateAndPrime'd on a different dispatcher), drop the stash but
  // still pay for the decompress + fanout work + ack the request.
  std::string session_id = *typed_req.session_id();

  // Capture inbound payload bytes for stashing — also pay for
  // decompress cost on the GlobalCPU thread.
  std::string ifr_payload = *typed_req.ifr_objects_serialized();

  auto context_ptr =
      std::make_shared<feedsim::RequestContext>(std::move(context));

  auto rankerPool = this_thread.srvCPUThreadPool;
  auto globalCpu = this_thread.cpuThreadPool;
  folly::EventBase* dispatcher_evb = this_thread.dispatcher_evb;
  ThreadData* this_thread_ptr = &this_thread;

  // Anchor on a SemiFuture so the move-only captures (ifr_payload) are
  // accepted, then hop to the ranker pool. folly::via(Executor*, Func) does
  // not accept lambdas that return a SemiFuture, and our inner lambda needs
  // to return a SemiFuture<std::string> from collectAll, so we use the
  // SemiFuture-anchored chain instead.
  folly::makeSemiFuture()
      .via(rankerPool.get())
      .thenValue([this_thread_ptr,
                  ifr_payload = std::move(ifr_payload),
                  globalCpu](folly::Unit) mutable {
        // Stage: decompress + small scoring pass on GlobalCPUThread.
        auto decompress_future = folly::via(
            globalCpu.get(),
            [ifr_payload = std::move(ifr_payload)]() mutable {
              std::string out;
              if (!ifr_payload.empty()) {
                try {
                  out = decompressPayload(ifr_payload);
                } catch (const std::exception&) {
                  out = std::move(ifr_payload);
                }
              }
              return out;
            });
        // Stage: small fanout — scale * 0.1 (researcher §4 row 5).
        auto fanout_future = issueOutboundFanout(
                                 *this_thread_ptr,
                                 args.rpc_fanout_scale_arg * 0.1)
                                 .thenValue([](int) { return folly::unit; });
        // folly::collectAll(Future, Future) returns
        // SemiFuture<tuple<Try<T1>, Try<T2>>>. SemiFuture chains with
        // deferValue (not thenValue, which is Future-only).
        return folly::collectAll(
                   std::move(decompress_future), std::move(fanout_future))
            .deferValue([](std::tuple<folly::Try<std::string>,
                                      folly::Try<folly::Unit>>&& results) {
              // Pull just the decompressed bytes out of the collectAll
              // result so the dispatcher hop carries only what it
              // needs.
              auto& decomp_try = std::get<0>(results);
              std::string decompressed;
              if (decomp_try.hasValue()) {
                decompressed = std::move(decomp_try.value());
              }
              return decompressed;
            });
      })
      .via(dispatcher_evb)
      .thenValue([this_thread_ptr,
                  session_id = std::move(session_id)](
                     std::string decompressed) {
        // Phase 6 (Issues 4+5 fix): NOW on the dispatcher thread. Safe
        // to read/mutate `sessions` and `session_id_to_query_id`.
        // Resolve session_id -> query_id; drop the stash if we don't
        // own this session.
        auto sit =
            this_thread_ptr->session_id_to_query_id.find(session_id);
        if (sit == this_thread_ptr->session_id_to_query_id.end()) {
          return folly::unit;
        }
        auto it = this_thread_ptr->sessions.find(sit->second);
        if (it == this_thread_ptr->sessions.end()) {
          return folly::unit;
        }
        constexpr size_t kMaxIfrPayloads = 4;
        if (it->second.ifr_payloads.size() >= kMaxIfrPayloads) {
          it->second.ifr_payloads.erase(it->second.ifr_payloads.begin());
        }
        it->second.ifr_payloads.push_back(std::move(decompressed));
        return folly::unit;
      })
      .thenValue([context_ptr](folly::Unit) {
        // Stay on dispatcher (or hop back if the previous returned us
        // elsewhere — folly::Future preserves the last via). Build +
        // send the ack.
        ranking::StreamIfrPriorityRankingResponse resp;
        resp.ack_code() = 0;
        sendThriftResponse(*context_ptr, resp);
      })
      .thenError(
          folly::tag_t<std::exception>{},
          [context_ptr](const std::exception& e) {
            std::cerr << "StreamIfrPriorityRanking handler error: " << e.what()
                      << std::endl;
            context_ptr->sendResponse(nullptr, 0);
          });
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

  // ThriftSrv.IO: inbound RPC IO loop. Renamed (was anonymous folly default).
  auto ioThreadPool = std::make_shared<folly::IOThreadPoolExecutor>(
      args.io_threads_arg,
      std::make_shared<folly::NamedThreadFactory>("ThriftSrv.IO"));

  // SREventBase: outbound-RPC EventBase pool. Carries the
  // issueOutboundFanout work to mock_services (Phase 5), replacing the
  // legacy srvIOThread pool's throw-away datagen + compression. Sized
  // 0.7 * nproc by default (matches CPL prod: 39 SREventBase threads on
  // a 52-logical-core host).
  const int srEventBaseThreads = (args.sr_event_base_threads_arg > 0)
      ? args.sr_event_base_threads_arg
      : std::max(1, (static_cast<int>(nproc) * 7) / 10);
  auto srEventBasePool = std::make_shared<folly::IOThreadPoolExecutor>(
      srEventBaseThreads,
      std::make_shared<folly::NamedThreadFactory>("SREventBase"));

  std::cout << "Thread pools (nproc=" << nproc << "): "
            << "GlobalCPUThread (folly singleton), "
            << "RANKER=" << rankerThreads << ", "
            << "SREventBase=" << srEventBaseThreads << ", "
            << "ThriftSrv.IO=" << args.io_threads_arg << std::endl;

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

  // Phase 6: register the 5 production-shaped inbound methods, each
  // wired to its real per-method handler (replaces the Phase 4 shims).
  // See ~/feedsim_v2/docs/phase6_researcher_notes.md §4 for the
  // per-handler stage breakdown and thread-pool routing.
  std::cout << "Registering Phase 6 per-method inbound handlers" << std::endl;
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

  // Background thread that periodically dumps the mock_services fanout
  // debug histograms to stderr so we can see how the per-RPC dispatch
  // and total fanout latencies evolve over the run. Detached so the
  // process exit will tear it down. 10s cadence keeps the log compact
  // but still picks up the warmup→steady-state transition.
  std::thread debug_dump_thread([]() {
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      g_fanout_total_us.dump("fanout_total");
      g_dispatch_us.dump("dispatch_per_rpc");
      g_sampled_lat_us.dump("sampled_latency_us");
    }
  });
  debug_dump_thread.detach();

  server.run();

  // One last dump after the server returns so the final state hits the
  // log even if the periodic timer was mid-sleep at shutdown.
  g_fanout_total_us.dump("fanout_total_final");
  g_dispatch_us.dump("dispatch_per_rpc_final");
  g_sampled_lat_us.dump("sampled_latency_us_final");

  return 0;
}

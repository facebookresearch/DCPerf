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
#include <cstring>
#include <iostream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <event2/event.h>

#include <folly/concurrency/ConcurrentHashMap.h>
#include <folly/executors/CPUThreadPoolExecutor.h>
#include <folly/executors/thread_factory/NamedThreadFactory.h>
#include <folly/futures/Future.h>
#include <folly/futures/Promise.h>

#include <thrift/lib/cpp2/protocol/CompactProtocol.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include "FeedSimDriver.h"
#include "FeedSimProtocol.h"

#include "DriverNodeRankCmdline.h"
#include "FeatureGenerator.h"
#include "RequestSizeSampler.h"
#include "RequestTypes.h"
#include "RpcDistRegistry.h"
#include "SilesiaLoader.h"

#include "if/gen-cpp2/ranking_types.h"

#include "utils.h"

static gengetopt_args_info args;

// Global Silesia corpus loader (shared across threads, read-only after init)
static std::unique_ptr<ranking::SilesiaLoader> g_silesia_loader;

// Global request size sampler (loaded from JSON, read-only after init)
static std::unique_ptr<ranking::RequestSizeSampler> g_req_size_sampler;

// Phase 6: rpc_dist.json registry for inbound percentile distributions
// (driver shapes typed-request bodies to match prod). Loaded only when
// --rpc_dist_json is given; nullptr otherwise.
static std::unique_ptr<ranking::RpcDistRegistry> g_rpc_dist_registry;

// Phase 6: per-driver-thread session orchestration executor. Sized
// num_threads (one logical session per driver thread, mirroring the
// legacy 1-cb-per-thread make-request loop). Named so Strobelight
// categorizes the sessions cleanly.
static std::shared_ptr<folly::CPUThreadPoolExecutor> g_session_pool;

const int kMaxRequestSize = 8192;
const int kRecomputeQPSPeriod = 1;  // Reduced from 5 to 1 second for faster feedback

// Simple random string generator (replaces oldisim/Util.h RandomString)
static std::string RandomString(size_t length) {
  static const char charset[] =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<size_t> dist(0, sizeof(charset) - 2);
  std::string str(length, 0);
  for (size_t i = 0; i < length; ++i) {
    str[i] = charset[dist(rng)];
  }
  return str;
}

struct ThreadData {
  std::string random_string;
  double qps_per_thread;
  uint64_t request_delay; // This is per thread
  feedsim::TestDriver *test_driver;
  event *recompute_qps_timer;

  // Client-side feature generation
  std::unique_ptr<ranking::FeatureGenerator> feature_generator;
  std::string serialized_request;  // Pre-allocated buffer for serialized request

  // Silesia story generation
  std::mt19937 silesia_rng;

  // Request size distribution sampling
  std::mt19937 req_size_rng;

  // Phase 6: per-thread session counter. Combined with thread_id to
  // form a unique 64-bit query_id (high 32 = thread, low 32 = counter).
  uint64_t session_counter = 0;
};

// Specific timer handler to recompute inter-request delays for QPS
void AddRecomputeDelayTimer(ThreadData &this_thread);
void RecomputeDelayTimerHandler(evutil_socket_t listener, int16_t flags,
                                void *arg);

// Declarations of handlers
void ThreadStartup(int thread_id,
                   feedsim::TestDriver &test_driver,
                   std::vector<ThreadData> &thread_data);
void MakeRequest(int thread_id, feedsim::TestDriver &test_driver,
                 std::vector<ThreadData> &thread_data);
void StartSessionLoop(int thread_id, feedsim::TestDriver &test_driver,
                      std::vector<ThreadData> &thread_data);

void AddRecomputeDelayTimer(ThreadData &this_thread) {
  timeval t = {kRecomputeQPSPeriod, 0};
  evtimer_add(this_thread.recompute_qps_timer, &t);
}

void RecomputeDelayTimerHandler(evutil_socket_t listener, int16_t flags,
                                void *arg) {
  ThreadData *this_thread = reinterpret_cast<ThreadData *>(arg);
  const feedsim::DriverStats &stats =
      this_thread->test_driver->getConnectionStats();

  // Get QPS for last stats period
  uint64_t now = feedsim::getTimeNano();
  double elapsed_secs = (now - stats.getStartTimeNano()) / 1000000000.0;
  if (elapsed_secs <= 0) {
    AddRecomputeDelayTimer(*this_thread);
    return;
  }

  // In session mode, --qps targets sessions/sec; in legacy mode it
  // targets requests/sec. Pick the right counter so the pacing
  // controller measures what the user asked to control.
  uint64_t cur_count;
  if (args.rpc_dist_json_given) {
    cur_count = stats.getSessionCount();
  } else {
    uint32_t request_type = (args.client_side_features_given ||
                             args.silesia_dir_given ||
                             args.req_size_dist_given)
        ? ranking::kDLRMRequestType
        : ranking::kPageRankRequestType;
    cur_count = stats.getQueryCount(request_type);
  }

  double measured_qps = static_cast<double>(cur_count) / elapsed_secs;

  // Compute target delay in microseconds
  double target_delay_us = 1000000.0 / this_thread->qps_per_thread;

  // Adjust delay using proportional feedback control
  // If measured_qps > target: increase delay to slow down
  // If measured_qps < target: decrease delay to speed up
  // Use a damping factor (0.5) to prevent oscillations
  double qps_ratio = measured_qps / this_thread->qps_per_thread;
  double damping = 0.5;
  double adjustment = 1.0 + damping * (qps_ratio - 1.0);

  // Clamp adjustment to prevent extreme values
  if (adjustment < 0.5) adjustment = 0.5;
  if (adjustment > 2.0) adjustment = 2.0;

  this_thread->request_delay = static_cast<uint64_t>(target_delay_us * adjustment);

  // Ensure minimum delay to prevent flooding
  if (this_thread->request_delay < 100) {
    this_thread->request_delay = 100;  // 100us minimum = 10000 QPS max per thread
  }

  AddRecomputeDelayTimer(*this_thread);
}

void ThreadStartup(int thread_id,
                   feedsim::TestDriver &test_driver,
                   std::vector<ThreadData> &thread_data) {
  ThreadData &this_thread = thread_data[thread_id];

  // Initialize random string with random bits
  this_thread.random_string = RandomString(kMaxRequestSize);

  // Store pointer to test_driver
  this_thread.test_driver = &test_driver;

  // Initialize client-side feature generator if enabled
  if (args.client_side_features_given) {
    ranking::FeatureGeneratorConfig config;
    config.batch_size = args.client_dlrm_batch_size_arg;
    config.num_dense_features = args.client_num_dense_features_arg;
    config.num_sparse_features = args.client_num_sparse_features_arg;
    config.seed = static_cast<unsigned>(args.client_feature_seed_arg);

    this_thread.feature_generator =
        std::make_unique<ranking::FeatureGenerator>(config, thread_id);
  }

  // Initialize Silesia RNG per thread
  if (g_silesia_loader && g_silesia_loader->isLoaded()) {
    this_thread.silesia_rng.seed(
        static_cast<unsigned>(std::random_device{}()) + thread_id);
  }

  // Initialize request size sampler RNG per thread
  if ((g_req_size_sampler && g_req_size_sampler->isLoaded()) ||
      g_rpc_dist_registry) {
    this_thread.req_size_rng.seed(
        static_cast<unsigned>(std::random_device{}()) + 0xDEADBEEF + thread_id);
  }

  // If user gave QPS target, initialize QPS modulation
  if (args.qps_arg != 0) {
    this_thread.qps_per_thread =
        (static_cast<double>(args.qps_arg)) / args.threads_arg;
    this_thread.recompute_qps_timer = evtimer_new(
        test_driver.getEventBase(), RecomputeDelayTimerHandler, &this_thread);
    AddRecomputeDelayTimer(this_thread);
    this_thread.request_delay = 1000000 / this_thread.qps_per_thread;
  } else {
    this_thread.request_delay = 0;
  }
}

// Add Silesia story snippets to a RankingRequest
void PopulateStories(ranking::RankingRequest& request,
                     ThreadData& this_thread) {
  if (!g_silesia_loader || !g_silesia_loader->isLoaded()) return;

  int num_stories = args.stories_per_request_arg;
  size_t min_size = static_cast<size_t>(args.story_size_min_arg);
  size_t max_size = static_cast<size_t>(args.story_size_max_arg);

  ranking::StoryBatch batch;
  std::vector<ranking::StoryContent>& stories = *batch.stories_ref();
  stories.reserve(num_stories);

  for (int i = 0; i < num_stories; ++i) {
    const uint8_t* data;
    size_t size;
    std::string filename;
    g_silesia_loader->getRandomSnippet(
        this_thread.silesia_rng, min_size, max_size, data, size, filename);

    ranking::StoryContent story;
    story.story_id() = static_cast<int64_t>(i);
    story.content() = std::string(reinterpret_cast<const char*>(data), size);
    story.source_file() = std::move(filename);
    story.content_length() = static_cast<int32_t>(size);
    stories.push_back(std::move(story));
  }

  request.story_batch() = std::move(batch);
}

// Fill `request.padding` with random-looking bytes so the serialized
// RankingRequest reaches `target_size`. Does nothing if the request already
// equals or exceeds the target. We use Silesia bytes when available
// (compression-realistic), otherwise a cheap PRNG over a stable buffer.
static void PadRequestToSize(ranking::RankingRequest& request,
                             ThreadData& this_thread,
                             size_t target_size) {
  // Serialize once to learn the base size.
  // Use cacheChainLength() so chainLength() is callable.
  folly::IOBufQueue probe_q(folly::IOBufQueue::cacheChainLength());
  apache::thrift::CompactSerializer::serialize(request, &probe_q);
  size_t base_size = probe_q.chainLength();

  if (base_size >= target_size) {
    return;
  }

  // Compact protocol encodes binary length as a varint (1-5 bytes for sizes
  // <= 4GB). Reserve 5 bytes of slack so we don't undershoot.
  constexpr size_t kVarintSlack = 5;
  size_t pad_len = target_size - base_size;
  if (pad_len > kVarintSlack) {
    pad_len -= kVarintSlack;
  } else {
    pad_len = 1;
  }

  std::string padding;
  padding.resize(pad_len);

  if (g_silesia_loader && g_silesia_loader->isLoaded()) {
    // Fill from Silesia snippets so the bytes are compression-realistic.
    size_t filled = 0;
    while (filled < pad_len) {
      const uint8_t* data = nullptr;
      size_t snippet_size = 0;
      std::string filename;
      g_silesia_loader->getRandomSnippet(
          this_thread.silesia_rng, 1024, 64 * 1024, data, snippet_size,
          filename);
      size_t copy_len = std::min(snippet_size, pad_len - filled);
      std::memcpy(padding.data() + filled, data, copy_len);
      filled += copy_len;
    }
  } else {
    // No Silesia - cheap PRNG fill (uniformly random bytes).
    std::uniform_int_distribution<int> byte_dist(0, 255);
    for (size_t i = 0; i < pad_len; ++i) {
      padding[i] = static_cast<char>(byte_dist(this_thread.req_size_rng));
    }
  }

  request.padding() = std::move(padding);
}

void MakeRequest(int thread_id, feedsim::TestDriver &test_driver,
                 std::vector<ThreadData> &thread_data) {
  ThreadData &this_thread = thread_data[thread_id];

  bool use_serialized_request = args.client_side_features_given ||
                                args.silesia_dir_given ||
                                args.req_size_dist_given;

  if (use_serialized_request) {
    // Serialized RankingRequest mode (client features, stories, or padding)
    ranking::RankingRequest request;
    request.request_id() = static_cast<int64_t>(thread_id);

    // Add DLRM features if client-side feature generation is enabled
    if (args.client_side_features_given) {
      int batch_size = args.client_dlrm_batch_size_arg;
      int num_inferences = args.client_dlrm_inferences_arg;

      auto dense_features =
          this_thread.feature_generator->generateDenseFeatures(batch_size);
      auto sparse_features =
          this_thread.feature_generator->generateSparseFeatures(batch_size);

      request.num_inferences() = num_inferences;

      ranking::DLRMFeatures features;
      features.batch_size() = batch_size;
      features.num_dense_features() = args.client_num_dense_features_arg;
      features.num_sparse_features() = args.client_num_sparse_features_arg;

      ranking::DenseFeatureVector dense_vec;
      dense_vec.reserve(dense_features.size());
      for (float f : dense_features) {
        dense_vec.push_back(static_cast<double>(f));
      }
      features.dense_features() = std::move(dense_vec);
      features.sparse_features() = std::move(sparse_features);

      request.dlrm_features() = std::move(features);
    }

    // Always set num_inferences for the server's DLRM inference stage
    if (!request.num_inferences().has_value()) {
      request.num_inferences() = args.client_dlrm_inferences_arg;
    }

    // Add Silesia stories if enabled
    if (args.silesia_dir_given) {
      PopulateStories(request, this_thread);
    }

    // Pad to target size sampled from the production distribution.
    if (g_req_size_sampler && g_req_size_sampler->isLoaded()) {
      size_t target =
          g_req_size_sampler->sample(this_thread.req_size_rng);
      if (target > 0) {
        PadRequestToSize(request, this_thread, target);
      }
    }

    // Serialize the request
    folly::IOBufQueue bufq;
    apache::thrift::CompactSerializer::serialize(request, &bufq);
    auto buf = bufq.move();
    buf->coalesce();

    // Send request with serialized RankingRequest
    test_driver.sendRequest(ranking::kDLRMRequestType,
                            reinterpret_cast<const char*>(buf->data()),
                            buf->length(),
                            this_thread.request_delay);
  } else {
    // Original mode: send random string payload
    test_driver.sendRequest(ranking::kPageRankRequestType,
                            this_thread.random_string.c_str(), 3000,
                            this_thread.request_delay);
  }
}

// ─── Phase 6: Session orchestration helpers ────────────────────────────────

namespace {

// Sample a target size from the inbound distribution; falls back to
// `fallback` when the registry isn't loaded for that method.
size_t sampleInboundReqSize(ranking::InboundIdx idx,
                            ThreadData& td,
                            size_t fallback) {
  if (g_rpc_dist_registry == nullptr) return fallback;
  const auto& sampler = g_rpc_dist_registry->inboundRequestSize(idx);
  if (!sampler.isLoaded()) return fallback;
  size_t s = sampler.sample(td.req_size_rng);
  return s > 0 ? s : fallback;
}

// Pad a binary field on a Thrift struct so the serialized struct hits
// roughly `target_size`. Generic version of PadRequestToSize that takes
// a setter for the binary field. Returns the actual serialized buffer
// (re-serialized after padding so callers don't pay double).
template <typename ThriftStruct, typename PadSetter>
std::string padBinaryToSize(
    ThriftStruct& s,
    PadSetter&& set_padding,
    ThreadData& td,
    size_t target_size) {
  // Probe: serialize once to learn base size.
  folly::IOBufQueue probe_q(folly::IOBufQueue::cacheChainLength());
  apache::thrift::CompactSerializer::serialize(s, &probe_q);
  size_t base_size = probe_q.chainLength();

  if (target_size > base_size) {
    constexpr size_t kVarintSlack = 5;
    size_t pad_len = target_size - base_size;
    pad_len = pad_len > kVarintSlack ? pad_len - kVarintSlack : 1;

    std::string padding;
    padding.resize(pad_len);
    if (g_silesia_loader && g_silesia_loader->isLoaded()) {
      size_t filled = 0;
      while (filled < pad_len) {
        const uint8_t* data = nullptr;
        size_t snippet_size = 0;
        std::string filename;
        g_silesia_loader->getRandomSnippet(
            td.silesia_rng, 1024, 64 * 1024, data, snippet_size, filename);
        size_t copy_len = std::min(snippet_size, pad_len - filled);
        std::memcpy(padding.data() + filled, data, copy_len);
        filled += copy_len;
      }
    } else {
      std::uniform_int_distribution<int> byte_dist(0, 255);
      for (size_t i = 0; i < pad_len; ++i) {
        padding[i] = static_cast<char>(byte_dist(td.req_size_rng));
      }
    }
    set_padding(s, std::move(padding));
  }

  // Final serialize.
  folly::IOBufQueue out_q(folly::IOBufQueue::cacheChainLength());
  apache::thrift::CompactSerializer::serialize(s, &out_q);
  auto buf = out_q.move();
  buf->coalesce();
  return std::string(
      reinterpret_cast<const char*>(buf->data()), buf->length());
}

// ─── Per-typed-request encoders ─────────────────────────────────────────────
//
// Each populate*() builds a typed thrift request, samples a target wire
// size from the inbound percentile distribution, pads the dominant
// binary field, and returns a serialized byte buffer ready to write.

std::string encodeCreateAndPrime(int64_t query_id, ThreadData& td) {
  ranking::CreateAndPrimeSessionRequest req;
  req.user_id() = static_cast<int64_t>(query_id) ^ 0xDEADBEEFCAFEBABEll;
  req.query_id() = query_id;
  req.caller_id() = "feedsim_driver";
  req.source() = "session_orchestrator";
  req.locale() = "en_US";
  req.client_query_id() = std::to_string(query_id);
  req.platform_type() = 1;
  req.browser_type() = 0;
  req.is_employee() = false;
  req.frontend_recv_timeout() = 2500;

  size_t target = sampleInboundReqSize(
      ranking::InboundIdx::kCreateAndPrimeSession, td, /*fallback=*/379);
  return padBinaryToSize(
      req,
      [](ranking::CreateAndPrimeSessionRequest& s, std::string&& p) {
        s.session_init_blob() = std::move(p);
      },
      td, target);
}

std::string encodeGetStories(
    int64_t query_id, const std::string& session_id, ThreadData& td) {
  ranking::GetStoriesRequest req;
  req.session_id() = session_id;
  req.query_id() = query_id;
  req.user_id() = static_cast<int64_t>(query_id) ^ 0xDEADBEEFCAFEBABEll;
  req.caller_id() = "feedsim_driver";
  req.source() = "session_orchestrator";
  req.locale() = "en_US";
  req.mobile_app_version() = "0.0.1";
  req.platform_type() = 1;
  req.browser_type() = 0;
  req.frontend_recv_timeout() = 2500;
  req.log_for_ranking() = false;
  req.is_employee() = false;
  req.caller_app_id() = 0;
  req.nth_retry() = 0;
  req.expected_ranking_model() = "default";
  req.push_phase() = "main";
  req.high_busy_contexts() = 0;

  size_t target = sampleInboundReqSize(
      ranking::InboundIdx::kGetStoriesUncompressed, td, /*fallback=*/2127642);
  return padBinaryToSize(
      req,
      [](ranking::GetStoriesRequest& s, std::string&& p) {
        s.settings_compressed() = std::move(p);
      },
      td, target);
}

std::string encodeStreamData(
    int64_t query_id, const std::string& session_id, int seq, ThreadData& td) {
  ranking::StreamDataRequest req;
  req.use_case() = ranking::StreamingUseCase::RANKING_DATA;
  req.session_id() = session_id;
  req.request_id() = (static_cast<int64_t>(query_id) << 8) | (seq & 0xFF);
  req.query_id() = query_id;

  size_t target = sampleInboundReqSize(
      ranking::InboundIdx::kStreamData, td, /*fallback=*/57838);
  return padBinaryToSize(
      req,
      [](ranking::StreamDataRequest& s, std::string&& p) {
        s.serialized_payload() = std::move(p);
      },
      td, target);
}

std::string encodeStreamIfrPriority(
    int64_t query_id, const std::string& session_id, ThreadData& td) {
  ranking::StreamIfrPriorityRankingRequest req;
  req.session_id() = session_id;
  req.request_id() = static_cast<int64_t>(query_id);
  req.eg_config_identifier() = "default";
  req.ifr_request_mode_www() = "main";

  size_t target = sampleInboundReqSize(
      ranking::InboundIdx::kStreamIfrPriorityRanking, td, /*fallback=*/948545);
  return padBinaryToSize(
      req,
      [](ranking::StreamIfrPriorityRankingRequest& s, std::string&& p) {
        s.ifr_objects_serialized() = std::move(p);
      },
      td, target);
}

std::string encodeGetAllStories(
    int64_t query_id, const std::string& session_id, ThreadData& td) {
  ranking::GetAllStoriesRequest req;
  req.session_id() = session_id;
  req.query_id() = query_id;
  req.caller_id() = "feedsim_driver";
  // Tiny (~55 bytes) — no padding needed; just serialize.
  folly::IOBufQueue out_q(folly::IOBufQueue::cacheChainLength());
  apache::thrift::CompactSerializer::serialize(req, &out_q);
  auto buf = out_q.move();
  buf->coalesce();
  return std::string(
      reinterpret_cast<const char*>(buf->data()), buf->length());
}

} // namespace

// ─── RunSession ─────────────────────────────────────────────────────────────
//
// Run one driver session per the prod multifeed_aggregator pipeline:
//   t=0     createAndPrimeSession           AWAIT
//   t≈3ms   getStoriesUncompressed          HOLD future
//   t≈3ms   streamData (parallel x N)       AWAIT
//   t≈410ms first-story = getStoriesUncompressed resolves (record latency)
//   t≈410ms getAllStories                   AWAIT
//
// Returns a SemiFuture<Unit> that resolves when getAllStories completes
// (or at any error along the way). The session loop in StartSessionLoop
// calls this and re-arms itself on completion to honor --qps pacing.
folly::SemiFuture<folly::Unit>
RunSession(int thread_id, feedsim::TestDriver& driver,
           std::vector<ThreadData>& thread_data) {
  ThreadData& td = thread_data[thread_id];
  uint64_t session_idx = td.session_counter++;
  int64_t query_id = (static_cast<int64_t>(thread_id) << 32) |
                     static_cast<int64_t>(session_idx & 0xFFFFFFFFULL);

  // ---- Step 1: createAndPrimeSession (await) ---------------------------
  std::string cap_buf = encodeCreateAndPrime(query_id, td);

  return driver
      .sendRequestAndAwait(
          ranking::kCreateAndPrimeSessionRequestType,
          cap_buf.data(),
          static_cast<uint32_t>(cap_buf.size()))
      .deferValue([&driver, &td, query_id, thread_id, &thread_data](
                      std::string cap_resp_bytes) {
        // Try to extract session_id from the typed response. On parse
        // failure (server sent an empty/dummy response), fall back to
        // a deterministic placeholder so the rest of the chain still
        // exercises the same call shapes.
        std::string session_id;
        if (!cap_resp_bytes.empty()) {
          try {
            ranking::CreateAndPrimeSessionResponse resp;
            folly::IOBuf buf(
                folly::IOBuf::WRAP_BUFFER,
                cap_resp_bytes.data(), cap_resp_bytes.size());
            apache::thrift::CompactSerializer::deserialize(&buf, resp);
            session_id = *resp.session_id_ref();
          } catch (const std::exception&) {
            // ignore; fall through to placeholder
          }
        }
        if (session_id.empty()) {
          session_id = std::to_string(query_id);
        }

        // ---- Step 2a: build + send getStoriesUncompressed (HOLD) ------
        std::string gs_buf = encodeGetStories(query_id, session_id, td);
        uint64_t t_gs_send = feedsim::getTimeNano();
        auto gs_future = driver.sendRequestAndAwait(
            ranking::kGetStoriesUncompressedRequestType,
            gs_buf.data(),
            static_cast<uint32_t>(gs_buf.size()));

        // ---- Step 2b: streamData × N (parallel) -----------------------
        int num_stream;
        if (args.streamdata_per_session_arg <= 0) {
          // Production-shaped: uniform [1,3].
          std::uniform_int_distribution<int> d(1, 3);
          num_stream = d(td.req_size_rng);
        } else {
          num_stream = args.streamdata_per_session_arg;
        }
        std::vector<folly::SemiFuture<std::string>> stream_futures;
        stream_futures.reserve(num_stream);
        for (int i = 0; i < num_stream; ++i) {
          std::string sd_buf = encodeStreamData(query_id, session_id, i, td);
          stream_futures.push_back(driver.sendRequestAndAwait(
              ranking::kStreamDataRequestType,
              sd_buf.data(),
              static_cast<uint32_t>(sd_buf.size())));
        }

        // Optional streamIfrPriorityRanking — coin flip per session.
        std::uniform_real_distribution<double> coin(0.0, 1.0);
        if (coin(td.req_size_rng) < args.stream_ifr_probability_arg) {
          std::string ifr_buf =
              encodeStreamIfrPriority(query_id, session_id, td);
          stream_futures.push_back(driver.sendRequestAndAwait(
              ranking::kStreamIfrPriorityRankingRequestType,
              ifr_buf.data(),
              static_cast<uint32_t>(ifr_buf.size())));
        }

        // ---- Step 2c: await all streamData/IFR acks -------------------
        return folly::collectAll(std::move(stream_futures))
            .deferValue([&driver, &td, query_id, session_id,
                         t_gs_send,
                         gs_future = std::move(gs_future)](
                            auto&& /*stream_results*/) mutable {
              // ---- Step 3: NOW await getStoriesUncompressed ----------
              return std::move(gs_future)
                  .deferValue([&driver, &td, query_id, session_id, t_gs_send](
                                  std::string /*gs_resp*/) {
                    uint64_t now = feedsim::getTimeNano();
                    driver.recordFirstStoryLatencyNs(now - t_gs_send);

                    // ---- Step 4: getAllStories (await) ---------------
                    std::string gas_buf =
                        encodeGetAllStories(query_id, session_id, td);
                    return driver
                        .sendRequestAndAwait(
                            ranking::kGetAllStoriesRequestType,
                            gas_buf.data(),
                            static_cast<uint32_t>(gas_buf.size()))
                        .deferValue([](std::string /*gas_resp*/) {
                          return folly::unit;
                        });
                  });
            });
      });
}

// Per-driver-thread session loop. Called from MakeRequest dispatch when
// session-mode is active. Issues one RunSession on g_session_pool, then
// re-arms the libevent QPS-pacing timer so the next session is launched
// at the rate dictated by --qps. We do not block the libevent thread on
// the SemiFuture chain — we attach the re-arm as a continuation.
void StartSessionLoop(int thread_id, feedsim::TestDriver& test_driver,
                      std::vector<ThreadData>& thread_data) {
  ThreadData& td = thread_data[thread_id];
  // Snapshot the pacing delay computed by RecomputeDelayTimerHandler.
  uint64_t delay_us = td.request_delay;

  // Phase 6 (Issue 1 fix): break the TestDriver::Impl::makeRequests
  // spin loop. Without setting next_request_delay_us synchronously
  // before returning to libevent, the do-while loop in makeRequests
  // would call this callback repeatedly until num_ready_connections
  // hits 0 — spawning unbounded sessions onto g_session_pool. The
  // async scheduleNextSession() that fires after RunSession completes
  // re-arms the pacing timer for the NEXT iteration; this synchronous
  // setter only terminates the current spin-loop pass.
  //
  // Use 1us as a sentinel non-zero value when --qps=0 so the loop
  // terminates after a single iteration; the pacing timer with 1us
  // fires immediately on the next libevent tick anyway.
  uint64_t loop_terminator_us = delay_us > 0 ? delay_us : 1;
  test_driver.setNextRequestDelayUs(loop_terminator_us);

  // Dispatch the session orchestration to the dedicated session pool so
  // we do not block the libevent thread on the SemiFuture chain.
  folly::via(g_session_pool.get(),
             [thread_id, &test_driver, &thread_data, delay_us]() {
               RunSession(thread_id, test_driver, thread_data)
                   .via(g_session_pool.get())
                   .thenValue([&test_driver, delay_us](folly::Unit) {
                     test_driver.recordSessionComplete();
                     // Re-arm the per-thread pacing timer to launch the
                     // next session. delay_us=0 means "fire immediately"
                     // (the libevent thread will call MakeRequest again
                     // straight away).
                     test_driver.scheduleNextSession(delay_us);
                   })
                   .thenError(folly::tag_t<std::exception>{},
                              [&test_driver, delay_us](const std::exception& e) {
                                std::cerr
                                    << "RunSession failed: " << e.what()
                                    << std::endl;
                                test_driver.scheduleNextSession(delay_us);
                              });
             });
}

int main(int argc, char **argv) {
  // Parse arguments
  if (cmdline_parser(argc, argv, &args) != 0) {
    std::cerr << "cmdline_parser failed" << std::endl;
    return 1;
  }

  // Check required arguments
  if (!args.server_given) {
    std::cerr << "--server must be specified." << std::endl;
    return 1;
  }

  // Load Silesia corpus if specified
  if (args.silesia_dir_given) {
    g_silesia_loader = std::make_unique<ranking::SilesiaLoader>();
    if (!g_silesia_loader->loadDirectory(args.silesia_dir_arg)) {
      std::cerr << "Failed to load Silesia corpus from: "
                << args.silesia_dir_arg << std::endl;
      return 1;
    }
    std::cout << "Silesia corpus loaded: " << g_silesia_loader->numFiles()
              << " files, " << (g_silesia_loader->totalSize() / (1024 * 1024))
              << " MB" << std::endl;
    std::cout << "  Stories per request: " << args.stories_per_request_arg
              << std::endl;
    std::cout << "  Story size: " << args.story_size_min_arg << "-"
              << args.story_size_max_arg << " bytes" << std::endl;
  }

  // Load request size distribution if specified
  if (args.req_size_dist_given) {
    g_req_size_sampler = std::make_unique<ranking::RequestSizeSampler>();
    if (!g_req_size_sampler->load(args.req_size_dist_arg, "req_size")) {
      std::cerr << "Failed to load request size distribution from: "
                << args.req_size_dist_arg << std::endl;
      return 1;
    }
    std::cout << "Request size distribution loaded from "
              << args.req_size_dist_arg << std::endl;
  }

  // Phase 6: load rpc_dist.json for inbound size shaping (session mode).
  if (args.rpc_dist_json_given) {
    g_rpc_dist_registry = std::make_unique<ranking::RpcDistRegistry>();
    if (!g_rpc_dist_registry->load(args.rpc_dist_json_arg)) {
      std::cerr << "Failed to load rpc_dist.json from: "
                << args.rpc_dist_json_arg << std::endl;
      return 1;
    }
    std::cout << "rpc_dist.json loaded for session-mode driver from "
              << args.rpc_dist_json_arg << std::endl;
    if (!g_rpc_dist_registry->isInboundFullyLoaded()) {
      std::cerr << "Warning: rpc_dist.json missing some inbound sections; "
                   "driver will use fixed-size fallbacks for those methods"
                << std::endl;
    }
  }

  auto host_port = ranking::utils::parseHostnameAndPort(args.server_arg);

  // Make storage for thread variables
  std::vector<ThreadData> thread_data(args.threads_arg);

  feedsim::FeedSimDriver driver_node(host_port.first, host_port.second);

  // Phase 6: spin up the session orchestration pool. One worker per
  // driver thread mirrors the legacy 1-cb-per-thread model. Named so
  // Strobelight categorizes the threads cleanly.
  if (args.rpc_dist_json_given) {
    g_session_pool = std::make_shared<folly::CPUThreadPoolExecutor>(
        args.threads_arg,
        std::make_shared<folly::NamedThreadFactory>("DriverSession"));

    // Phase 6 (Issue 3 fix): hook g_session_pool teardown into
    // FeedSimDriver::shutdown() BEFORE event_bases are freed. Without
    // this, in-flight session continuations can land an evtimer_add on
    // a freed event_base. The pre-teardown callback fires after every
    // TestDriver::running flag is flipped to false, so any continuation
    // that reaches scheduleNextSession() at this point already bails
    // out; stopping+joining the pool here drains the rest.
    driver_node.setPreTeardownCallback([]() {
      if (g_session_pool) {
        g_session_pool->stop();
        g_session_pool->join();
      }
    });
  }

  driver_node.setThreadStartupCallback(
      std::bind(ThreadStartup, std::placeholders::_1, std::placeholders::_2,
                std::ref(thread_data)));

  // Phase 6: when session mode is on, dispatch RunSession via the
  // libevent make-request callback — exactly the same hook the legacy
  // per-request MakeRequest used. This keeps the existing pacing /
  // backpressure machinery (markConnectionNotReady, recompute timer)
  // intact: libevent fires make_request_cb, which kicks off one async
  // session; the SemiFuture chain re-arms the timer when done.
  if (args.rpc_dist_json_given) {
    driver_node.setMakeRequestCallback(
        std::bind(StartSessionLoop, std::placeholders::_1,
                  std::placeholders::_2, std::ref(thread_data)));
  } else {
    driver_node.setMakeRequestCallback(
        std::bind(MakeRequest, std::placeholders::_1, std::placeholders::_2,
                  std::ref(thread_data)));
  }

  // Register only the request type that will be used
  // This ensures stats are collected for a single type, avoiding output parsing issues
  if (args.rpc_dist_json_given) {
    // Session mode: register the dominant inbound type (getStories) so
    // the printed "Stats for node under test, type N" header has a
    // meaningful value. The DriverStats counters are not actually
    // sharded by type today, so this is purely cosmetic.
    driver_node.registerRequestType(
        ranking::kGetStoriesUncompressedRequestType);
  } else if (args.client_side_features_given || args.silesia_dir_given ||
             args.req_size_dist_given) {
    driver_node.registerRequestType(ranking::kDLRMRequestType);
  } else {
    driver_node.registerRequestType(ranking::kPageRankRequestType);
  }

  // Enable remote monitoring
  driver_node.enableMonitoring(args.monitor_port_arg);

  // Log client-side feature generation mode
  if (args.client_side_features_given) {
    std::cout << "Client-side feature generation enabled:" << std::endl;
    std::cout << "  Batch size: " << args.client_dlrm_batch_size_arg << std::endl;
    std::cout << "  Inferences per request: " << args.client_dlrm_inferences_arg << std::endl;
    std::cout << "  Dense features: " << args.client_num_dense_features_arg << std::endl;
    std::cout << "  Sparse features: " << args.client_num_sparse_features_arg << std::endl;
    std::cout << "  Seed: " << args.client_feature_seed_arg << std::endl;
  }

  // Log Silesia mode
  if (args.silesia_dir_given) {
    std::cout << "Silesia story generation enabled:" << std::endl;
    std::cout << "  Directory: " << args.silesia_dir_arg << std::endl;
    std::cout << "  Stories per request: " << args.stories_per_request_arg
              << std::endl;
    std::cout << "  Story size: " << args.story_size_min_arg << "-"
              << args.story_size_max_arg << " bytes" << std::endl;
  }

  if (args.rpc_dist_json_given) {
    std::cout << "Phase 6 session mode enabled:" << std::endl;
    std::cout << "  streamdata per session: "
              << args.streamdata_per_session_arg
              << " (0 = uniform[1,3])" << std::endl;
    std::cout << "  streamIfr probability: "
              << args.stream_ifr_probability_arg << std::endl;
  }

  driver_node.run(args.threads_arg, args.affinity_given, args.connections_arg,
                  args.depth_arg);

  // Tear down the session pool so its threads exit cleanly before the
  // process returns from main.
  if (g_session_pool) {
    g_session_pool->stop();
    g_session_pool.reset();
  }

  return 0;
}

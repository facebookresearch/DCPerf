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

#include <event2/event.h>

#include <thrift/lib/cpp2/protocol/CompactProtocol.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include "FeedSimDriver.h"
#include "FeedSimProtocol.h"

#include "DriverNodeRankCmdline.h"
#include "FeatureGenerator.h"
#include "RequestSizeSampler.h"
#include "RequestTypes.h"
#include "SilesiaLoader.h"

#include "if/gen-cpp2/ranking_types.h"

#include "utils.h"

static gengetopt_args_info args;

// Global Silesia corpus loader (shared across threads, read-only after init)
static std::unique_ptr<ranking::SilesiaLoader> g_silesia_loader;

// Global request size sampler (loaded from JSON, read-only after init)
static std::unique_ptr<ranking::RequestSizeSampler> g_req_size_sampler;

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

  // Use the appropriate request type for QPS calculation
  uint32_t request_type = (args.client_side_features_given ||
                           args.silesia_dir_given ||
                           args.req_size_dist_given)
      ? ranking::kDLRMRequestType
      : ranking::kPageRankRequestType;

  double measured_qps = static_cast<double>(stats.getQueryCount(request_type))
                        / elapsed_secs;

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
  if (g_req_size_sampler && g_req_size_sampler->isLoaded()) {
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

  auto host_port = ranking::utils::parseHostnameAndPort(args.server_arg);

  // Make storage for thread variables
  std::vector<ThreadData> thread_data(args.threads_arg);

  feedsim::FeedSimDriver driver_node(host_port.first, host_port.second);

  driver_node.setThreadStartupCallback(
      std::bind(ThreadStartup, std::placeholders::_1, std::placeholders::_2,
                std::ref(thread_data)));
  driver_node.setMakeRequestCallback(
      std::bind(MakeRequest, std::placeholders::_1, std::placeholders::_2,
                std::ref(thread_data)));

  // Register only the request type that will be used
  // This ensures stats are collected for a single type, avoiding output parsing issues
  if (args.client_side_features_given || args.silesia_dir_given ||
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

  driver_node.run(args.threads_arg, args.affinity_given, args.connections_arg,
                  args.depth_arg);

  return 0;
}

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

#include "MockServiceHandler.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>
#include <thread>
#include <utility>

#include <gflags/gflags.h>

#include <folly/executors/GlobalExecutor.h>
#include <folly/futures/Future.h>
#include <folly/portability/Asm.h>

#include "LatencyHistogram.h"

// rpc_dist.json contains very long-tail latencies (p99 ~8s, max ~28s)
// because the production aggregator's outbound RPCs occasionally hit
// retries / queueing / GC pauses; reproducing those raw values inside
// the mock makes the bench unusable (every fanout blocks on the
// slowest call). The flags below let us shape the simulated delay so
// it stays within a useful operating range without losing the
// per-method latency mix. Defined here (where they're consumed) so the
// mock_service_handler cpp_library links cleanly on its own; declared
// in MockServiceMain.cc for the startup LOG line.
DEFINE_int32(
    latency_cap_us,
    200000,
    "Cap the requested per-call latency at this value (us). Defaults to "
    "200ms so the slowest per-RPC sleep is bounded; set to 0 to disable.");
DEFINE_int32(
    latency_offset_us,
    0,
    "Subtract this many us from the requested latency to compensate for "
    "intrinsic RPC-stack overhead the client already pays for. Defaults "
    "to 0; tune empirically based on the gap between the leaf-side "
    "dispatch_per_rpc histogram and the mock-handler actual latency.");
DEFINE_int32(
    latency_skip_threshold_us,
    100,
    "If the effective latency (after cap + offset) is below this many us, "
    "skip the spin/sleep entirely and respond immediately. Avoids paying "
    "for spin-wait jitter on requests whose modeled budget is already "
    "comparable to the natural RPC round trip.");

namespace mock_services {

// Debug histograms exposed to MockServiceMain so it can dump them on
// a periodic timer. Defined in this TU because runSimulatedRpc records
// into them on every call. requested_us = the latency_us argument
// supplied by the client (sampled from rpc_dist.json). actual_us =
// elapsed wall time inside runSimulatedRpc, including spin/sleep and
// response generation. effective_us = the post-cap/offset/threshold
// value actually used to drive the sleep -- so we can see how much the
// shaping flags trim off the long tail.
feedsim::LatencyHistogram g_handler_requested_us;
feedsim::LatencyHistogram g_handler_actual_us;
feedsim::LatencyHistogram g_handler_effective_us;

namespace {

// Spin-vs-sleep cutoff. folly::futures::sleep uses the global timekeeper which
// has ~tens-of-microseconds wakeup jitter; for very short latencies we burn
// the IO thread to keep RPC-stack samples on-CPU as they would be in prod.
constexpr int32_t kSpinThresholdUs = 200;

// Cap on response payload size. Largest sampled response in rpc_dist.json is
// ~5 MB at streamData p95; 16 MB leaves headroom while preventing a malformed
// or buggy client from triggering bad_alloc on small-RAM hosts.
constexpr uint32_t kMaxResponseSize = 16 * 1024 * 1024;

thread_local std::mt19937 tlRng{std::random_device{}()};

// Free function so async continuations can use it without capturing `this`.
// Takes a shared_ptr by value so the SilesiaLoader outlives the continuation
// even if the handler is destroyed.
std::unique_ptr<std::string> generateResponseBytesStandalone(
    const std::shared_ptr<ranking::SilesiaLoader>& silesia,
    uint32_t response_size) {
  response_size = std::min(response_size, kMaxResponseSize);
  auto buf = std::make_unique<std::string>();
  buf->resize(response_size);
  if (response_size == 0 || silesia == nullptr || !silesia->isLoaded()) {
    return buf;
  }
  char* dst = buf->data();
  uint32_t remaining = response_size;
  while (remaining > 0) {
    const uint8_t* snippet = nullptr;
    size_t snippet_size = 0;
    std::string filename;
    silesia->getRandomSnippet(
        tlRng, /*min_size=*/1, remaining, snippet, snippet_size, filename);
    if (snippet_size == 0) {
      std::memset(dst, 0, remaining);
      break;
    }
    std::memcpy(dst, snippet, snippet_size);
    dst += snippet_size;
    remaining -= snippet_size;
  }
  return buf;
}

} // namespace

MockServiceHandler::MockServiceHandler(
    std::shared_ptr<ranking::SilesiaLoader> silesia)
    : silesia_(std::move(silesia)) {}

uint32_t MockServiceHandler::parseResponseSize(const std::string* request) {
  if (request == nullptr || request->size() < sizeof(uint32_t)) {
    return 0;
  }
  uint32_t be;
  std::memcpy(&be, request->data(), sizeof(uint32_t));
  return __builtin_bswap32(be);
}

std::unique_ptr<std::string> MockServiceHandler::generateResponseBytes(
    uint32_t response_size) {
  return generateResponseBytesStandalone(silesia_, response_size);
}

folly::SemiFuture<std::unique_ptr<std::string>>
MockServiceHandler::runSimulatedRpc(
    std::unique_ptr<std::string> request,
    int32_t latency_us) {
  uint32_t response_size = parseResponseSize(request.get());
  request.reset();

  // Debug: record the requested latency before any clamp/spin/sleep.
  g_handler_requested_us.record(static_cast<uint64_t>(std::max(0, latency_us)));
  uint64_t handler_start_us = feedsim::nowUs();

  // Shape the simulated delay: cap the head, subtract the natural
  // RPC-stack overhead the client already pays for, and skip the wait
  // entirely if the residual is below the configured floor (avoids
  // burning CPU on spin-jitter for sub-100us samples).
  int32_t effective_us = latency_us;
  if (FLAGS_latency_cap_us > 0 && effective_us > FLAGS_latency_cap_us) {
    effective_us = FLAGS_latency_cap_us;
  }
  effective_us = std::max(0, effective_us - FLAGS_latency_offset_us);
  g_handler_effective_us.record(static_cast<uint64_t>(effective_us));

  if (effective_us <= 0 || effective_us < FLAGS_latency_skip_threshold_us) {
    g_handler_actual_us.record(feedsim::nowUs() - handler_start_us);
    return folly::makeSemiFuture(generateResponseBytes(response_size));
  }
  if (effective_us < kSpinThresholdUs) {
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::microseconds(effective_us);
    while (std::chrono::steady_clock::now() < deadline) {
      folly::asm_volatile_pause();
    }
    g_handler_actual_us.record(feedsim::nowUs() - handler_start_us);
    return folly::makeSemiFuture(generateResponseBytes(response_size));
  }
  auto silesia = silesia_;
  return folly::futures::sleep(std::chrono::microseconds(effective_us))
      .via(folly::getGlobalCPUExecutor().get())
      .thenValue([silesia, response_size, handler_start_us](folly::Unit) {
        auto resp = generateResponseBytesStandalone(silesia, response_size);
        g_handler_actual_us.record(feedsim::nowUs() - handler_start_us);
        return resp;
      })
      .semi();
}

#define MOCK_SERVICES_DEFINE_METHOD(NAME)                                     \
  folly::SemiFuture<std::unique_ptr<std::string>>                             \
  MockServiceHandler::semifuture_##NAME(                                      \
      std::unique_ptr<std::string> request, int32_t latency_us) {             \
    return runSimulatedRpc(std::move(request), latency_us);                   \
  }

MOCK_SERVICES_DEFINE_METHOD(mcGet)
MOCK_SERVICES_DEFINE_METHOD(mcLeaseGet)
MOCK_SERVICES_DEFINE_METHOD(mcSet)
MOCK_SERVICES_DEFINE_METHOD(fetchTopKEntitiesRequest)
MOCK_SERVICES_DEFINE_METHOD(getActionStreamsRequestCompressed2)
MOCK_SERVICES_DEFINE_METHOD(getObjectsFromQueries)
MOCK_SERVICES_DEFINE_METHOD(getSerializedObjects)
MOCK_SERVICES_DEFINE_METHOD(getStatus)
MOCK_SERVICES_DEFINE_METHOD(runFullyRemotePrediction)
MOCK_SERVICES_DEFINE_METHOD(getActionStreamsCompressed2)
MOCK_SERVICES_DEFINE_METHOD(runModelMethod)
MOCK_SERVICES_DEFINE_METHOD(edsMultiGet)
MOCK_SERVICES_DEFINE_METHOD(fetchCandidateScoreRequest)
MOCK_SERVICES_DEFINE_METHOD(mcLeaseSet)
MOCK_SERVICES_DEFINE_METHOD(prefixScan)
MOCK_SERVICES_DEFINE_METHOD(fetchEntityFeatures)
MOCK_SERVICES_DEFINE_METHOD(getUserConsents)
MOCK_SERVICES_DEFINE_METHOD(fciGet)
MOCK_SERVICES_DEFINE_METHOD(multiget)
MOCK_SERVICES_DEFINE_METHOD(FbkeyPointGetRequest)

#undef MOCK_SERVICES_DEFINE_METHOD

} // namespace mock_services

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

#include <folly/executors/GlobalExecutor.h>
#include <folly/futures/Future.h>
#include <folly/portability/Asm.h>

namespace mock_services {

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

  if (latency_us <= 0) {
    return folly::makeSemiFuture(generateResponseBytes(response_size));
  }
  if (latency_us < kSpinThresholdUs) {
    auto deadline = std::chrono::steady_clock::now() +
        std::chrono::microseconds(latency_us);
    while (std::chrono::steady_clock::now() < deadline) {
      folly::asm_volatile_pause();
    }
    return folly::makeSemiFuture(generateResponseBytes(response_size));
  }
  auto silesia = silesia_;
  return folly::futures::sleep(std::chrono::microseconds(latency_us))
      .via(folly::getGlobalCPUExecutor().get())
      .thenValue([silesia, response_size](folly::Unit) {
        return generateResponseBytesStandalone(silesia, response_size);
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

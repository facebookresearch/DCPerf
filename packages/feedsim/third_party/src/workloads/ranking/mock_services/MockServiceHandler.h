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

#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <folly/futures/Future.h>

#include "cea/chips/benchpress/packages/feedsim/third_party/src/workloads/ranking/mock_services/gen-cpp2/MockService.h"

#include "SilesiaLoader.h"

namespace mock_services {

class MockServiceHandler
    : public apache::thrift::ServiceHandler<MockService> {
 public:
  explicit MockServiceHandler(std::shared_ptr<ranking::SilesiaLoader> silesia);
  ~MockServiceHandler() override = default;

  folly::SemiFuture<std::unique_ptr<std::string>> semifuture_mcGet(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>> semifuture_mcLeaseGet(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>> semifuture_mcSet(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>>
  semifuture_fetchTopKEntitiesRequest(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>>
  semifuture_getActionStreamsRequestCompressed2(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>>
  semifuture_getObjectsFromQueries(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>>
  semifuture_getSerializedObjects(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>> semifuture_getStatus(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>>
  semifuture_runFullyRemotePrediction(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>>
  semifuture_getActionStreamsCompressed2(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>> semifuture_runModelMethod(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>> semifuture_edsMultiGet(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>>
  semifuture_fetchCandidateScoreRequest(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>> semifuture_mcLeaseSet(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>> semifuture_prefixScan(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>>
  semifuture_fetchEntityFeatures(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>> semifuture_getUserConsents(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>> semifuture_fciGet(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>> semifuture_multiget(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;
  folly::SemiFuture<std::unique_ptr<std::string>>
  semifuture_FbkeyPointGetRequest(
      std::unique_ptr<std::string> request,
      int32_t latency_us) override;

 private:
  // Parses the leading uint32_t big-endian response_size from the request
  // payload. Returns 0 if the request is too small (handled as empty
  // response). See MockService.thrift for the wire contract.
  static uint32_t parseResponseSize(const std::string* request);

  // Build a response of `response_size` bytes from the Silesia corpus.
  std::unique_ptr<std::string> generateResponseBytes(uint32_t response_size);

  // Common implementation for all 20 methods. Shorter latencies (<200us) burn
  // CPU on the IO thread to keep rpc-stack samples on-CPU; longer latencies
  // sleep on the global timekeeper.
  folly::SemiFuture<std::unique_ptr<std::string>> runSimulatedRpc(
      std::unique_ptr<std::string> request,
      int32_t latency_us);

  std::shared_ptr<ranking::SilesiaLoader> silesia_;
};

} // namespace mock_services

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

#include "MockServicesClient.h"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#include <folly/SocketAddress.h>
#include <folly/io/async/AsyncSocket.h>

#include <thrift/lib/cpp2/async/RocketClientChannel.h>

#include "mock_services/gen-cpp2/MockServiceAsyncClient.h"

namespace ranking {

MockServicesClient::MockServicesClient(
    folly::EventBase* evb, const std::string& host, uint16_t port)
    : evb_(evb) {
  if (evb_ == nullptr) {
    throw std::invalid_argument(
        "MockServicesClient: EventBase must not be null");
  }

  // RocketClientChannel must be created on the EventBase thread. Use
  // runInEventBaseThreadAndWait so this constructor remains usable from
  // any thread (typically the main thread during ThreadStartup).
  evb_->runInEventBaseThreadAndWait([this, &host, port]() {
    folly::SocketAddress addr(host, port, /*allowNameLookup=*/true);
    folly::AsyncSocket::UniquePtr socket(
        new folly::AsyncSocket(evb_, addr));
    auto channel =
        apache::thrift::RocketClientChannel::newChannel(std::move(socket));
    client_ =
        std::make_unique<mock_services::MockServiceAsyncClient>(
            std::move(channel));
  });

  // Synchronously verify connectivity by issuing a tiny probe RPC against
  // getStatus (lightest method per rpc_dist.json: p05 ~259us, smallest
  // payloads). folly::AsyncSocket(evb, addr) only *initiates* an async
  // connect -- it does NOT throw on connect refused / host unreachable,
  // so without this probe a misconfigured deployment would silently leave
  // mock_client non-null and every per-RPC future would fail at request
  // time, getting swallowed to 0 by the .thenError handler in
  // issueOutboundFanout (LeafNodeRank.cc). That bypasses I/O simulation
  // entirely. Probing here surfaces the failure as a constructor
  // exception, which the existing try/catch in ThreadStartup
  // (LeafNodeRank.cc) catches to reset mock_client and fall back to the
  // legacy folly::futures::sleep path -- matching the regression-safety
  // contract documented in phase5_researcher_notes.md.
  //
  // CRITICAL: the probe MUST be issued from the constructor's caller
  // thread, NOT from inside runInEventBaseThreadAndWait. The EventBase
  // thread is the only thread that can drive socket connect/IO events
  // and the Rocket handshake; if we block it via getTry() inside the
  // EventBase loop, the connect POLLOUT and handshake events never get
  // processed and the probe deadlocks until the within() timer fires
  // from the separate folly timer wheel. RequestChannel's send-path
  // auto-dispatches the call onto the channel's EventBase
  // (fbcode/thrift/lib/cpp2/async/RequestChannel.h:415-424), so calling
  // semifuture_getStatus() from any thread is safe; only the wait must
  // be on a different thread than the EventBase.
  std::string probe(4, '\0');
  writeBigEndianResponseSize(probe.data(), 4);
  auto result = client_->semifuture_getStatus(probe, /*latency_us=*/0)
                    .within(std::chrono::seconds(30))
                    .getTry();
  if (result.hasException()) {
    // Reset client_ on the EventBase thread before throwing, so the
    // partially-constructed AsyncClient is destroyed on the correct
    // thread (channel/socket are thread-affine).
    evb_->runInEventBaseThreadAndWait([this]() { client_.reset(); });
    std::rethrow_exception(result.exception().to_exception_ptr());
  }
}

MockServicesClient::~MockServicesClient() {
  // The AsyncClient and its channel must be destroyed on the EventBase
  // thread. Use runInEventBaseThreadAndWait to guarantee that even when
  // the destructor runs from a different thread (e.g., during process
  // shutdown).
  if (evb_ != nullptr && client_) {
    evb_->runInEventBaseThreadAndWait([this]() { client_.reset(); });
  }
}

folly::SemiFuture<std::string> MockServicesClient::dispatchByEnum(
    MethodIdx idx, const std::string& request, int32_t latency_us) {
  // Compile-time switch over MethodIdx. Each case calls the named
  // generated semifuture_<method>() so Strobelight sees distinct
  // AsyncClient::send_<method> symbols — the whole reason we declared
  // 20 separate Thrift methods rather than a single generic call().
  switch (idx) {
    case MethodIdx::mcGet:
      return client_->semifuture_mcGet(request, latency_us);
    case MethodIdx::mcLeaseGet:
      return client_->semifuture_mcLeaseGet(request, latency_us);
    case MethodIdx::mcSet:
      return client_->semifuture_mcSet(request, latency_us);
    case MethodIdx::fetchTopKEntitiesRequest:
      return client_->semifuture_fetchTopKEntitiesRequest(request, latency_us);
    case MethodIdx::getActionStreamsRequestCompressed2:
      return client_->semifuture_getActionStreamsRequestCompressed2(
          request, latency_us);
    case MethodIdx::getObjectsFromQueries:
      return client_->semifuture_getObjectsFromQueries(request, latency_us);
    case MethodIdx::getSerializedObjects:
      return client_->semifuture_getSerializedObjects(request, latency_us);
    case MethodIdx::getStatus:
      return client_->semifuture_getStatus(request, latency_us);
    case MethodIdx::runFullyRemotePrediction:
      return client_->semifuture_runFullyRemotePrediction(request, latency_us);
    case MethodIdx::getActionStreamsCompressed2:
      return client_->semifuture_getActionStreamsCompressed2(
          request, latency_us);
    case MethodIdx::runModelMethod:
      return client_->semifuture_runModelMethod(request, latency_us);
    case MethodIdx::edsMultiGet:
      return client_->semifuture_edsMultiGet(request, latency_us);
    case MethodIdx::fetchCandidateScoreRequest:
      return client_->semifuture_fetchCandidateScoreRequest(
          request, latency_us);
    case MethodIdx::mcLeaseSet:
      return client_->semifuture_mcLeaseSet(request, latency_us);
    case MethodIdx::prefixScan:
      return client_->semifuture_prefixScan(request, latency_us);
    case MethodIdx::fetchEntityFeatures:
      return client_->semifuture_fetchEntityFeatures(request, latency_us);
    case MethodIdx::getUserConsents:
      return client_->semifuture_getUserConsents(request, latency_us);
    case MethodIdx::fciGet:
      return client_->semifuture_fciGet(request, latency_us);
    case MethodIdx::multiget:
      return client_->semifuture_multiget(request, latency_us);
    case MethodIdx::FbkeyPointGetRequest:
      return client_->semifuture_FbkeyPointGetRequest(request, latency_us);
    case MethodIdx::COUNT:
      break;
  }
  // Unreachable; defensive fallback.
  return folly::makeSemiFuture<std::string>(
      std::runtime_error("MockServicesClient::dispatchByEnum: invalid idx"));
}

} // namespace ranking

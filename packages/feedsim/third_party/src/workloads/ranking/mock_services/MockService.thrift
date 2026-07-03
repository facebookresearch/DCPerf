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

// Mock RPC fanout target for FeedSim's LeafNodeRank. Twenty methods, one per
// outbound RPC type observed in the production multifeed_aggregator profile
// (see ~/feedsim_v2/profiles/rpc_dist.json).
//
// All twenty share the same wire signature; the per-method names exist solely
// so Strobelight can attribute samples to each (the generated symbols are
// distinct: MockServiceSvIf::async_eb_<method>, AsyncClient::send_<method>,
// etc.). The handler body is a single shared function; payload sizes and
// latencies are sampled by the caller from the same percentile distribution.
//
// Wire contract: the first 4 bytes of `request` are a big-endian uint32_t
// response_size; the rest is opaque padding sized to the request percentile.
// The server sleeps/spins for `latency_us` and replies with `response_size`
// bytes copied from the Silesia corpus.
//
// Memcache note: mcGet, mcLeaseGet, mcSet, mcLeaseSet are wrapped as Thrift
// methods even though prod uses the memcache binary protocol on the wire.
// Reproducing the memcache framing buys no signal for RPC-stack CPU
// attribution — which is what we are calibrating — and would multiply
// scaffolding for no measurable difference in the categories of interest.

namespace cpp2 mock_services

service MockService {
  binary mcGet(1: binary request, 2: i32 latency_us);
  binary mcLeaseGet(1: binary request, 2: i32 latency_us);
  binary mcSet(1: binary request, 2: i32 latency_us);
  binary fetchTopKEntitiesRequest(1: binary request, 2: i32 latency_us);
  binary getActionStreamsRequestCompressed2(
    1: binary request,
    2: i32 latency_us,
  );
  binary getObjectsFromQueries(1: binary request, 2: i32 latency_us);
  binary getSerializedObjects(1: binary request, 2: i32 latency_us);
  binary getStatus(1: binary request, 2: i32 latency_us);
  binary runFullyRemotePrediction(1: binary request, 2: i32 latency_us);
  binary getActionStreamsCompressed2(1: binary request, 2: i32 latency_us);
  binary runModelMethod(1: binary request, 2: i32 latency_us);
  binary edsMultiGet(1: binary request, 2: i32 latency_us);
  binary fetchCandidateScoreRequest(1: binary request, 2: i32 latency_us);
  binary mcLeaseSet(1: binary request, 2: i32 latency_us);
  binary prefixScan(1: binary request, 2: i32 latency_us);
  binary fetchEntityFeatures(1: binary request, 2: i32 latency_us);
  binary getUserConsents(1: binary request, 2: i32 latency_us);
  binary fciGet(1: binary request, 2: i32 latency_us);
  binary multiget(1: binary request, 2: i32 latency_us);
  binary FbkeyPointGetRequest(1: binary request, 2: i32 latency_us);
}

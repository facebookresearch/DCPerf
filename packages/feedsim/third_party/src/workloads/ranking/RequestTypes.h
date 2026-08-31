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

#ifndef REQUEST_TYPES_H
#define REQUEST_TYPES_H

#include <cstdint>

namespace ranking {

// Request type for DLRM ranking requests (non-session mode).
static const int kDLRMRequestType = 1;

// Phase 4: production-shaped multifeed aggregator inbound methods. Type
// IDs 0x10..0x14 are dispatched by FeedSimServer::registerQueryCallback
// to the per-method shim handlers in LeafNodeRank.cc. See Phase 4
// researcher notes section 4 for the dispatch design.
constexpr uint32_t kCreateAndPrimeSessionRequestType = 0x10;
constexpr uint32_t kGetStoriesUncompressedRequestType = 0x11;
constexpr uint32_t kGetAllStoriesRequestType = 0x12;
constexpr uint32_t kStreamDataRequestType = 0x13;
constexpr uint32_t kStreamIfrPriorityRankingRequestType = 0x14;

} // namespace ranking

#endif // REQUEST_TYPES_H

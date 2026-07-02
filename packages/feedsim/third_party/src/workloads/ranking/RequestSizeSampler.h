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

// Phase 5: RequestSizeSampler is now a thin alias for PercentileSampler,
// which generalizes the percentile-loading logic so RpcDistRegistry can
// load any of the 60 distributions in rpc_dist.json. The legacy single-
// distribution + prefixed-keys API used by DriverNodeRank
// (--request_size_distribution) is preserved verbatim through
// PercentileSampler::load(json_path, field_prefix).

#include "PercentileSampler.h"

namespace ranking {

using RequestSizeSampler = PercentileSampler;

} // namespace ranking

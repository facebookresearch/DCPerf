/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <glog/logging.h>
#include <unordered_map>

#include <fb303/ExportType.h>
#include <fb303/ThreadCachedServiceData.h>

#include "cea/chips/adsim/cpp2/server/dwarfs/Kernel.h"

namespace facebook::cea::chips::adsim {

DEFINE_timeseries(fakeio_nfired, "fakeio_nfired", fb303::SUM);
DEFINE_timeseries(ibrun_nfired, "ibrun_nfired", fb303::SUM);
DEFINE_timeseries(gemm_nfired, "gemm_nfired", fb303::SUM);
DEFINE_timeseries(compress_nfired, "compress_nfired", fb303::SUM);
DEFINE_timeseries(decompress_nfired, "decompress_nfired", fb303::SUM);
DEFINE_timeseries(serialize_nfired, "serialize_nfired", fb303::SUM);
DEFINE_timeseries(deserialize_nfired, "serialize_nfired", fb303::SUM);
DEFINE_timeseries(hashmap_nfired, "hashmap_nfired", fb303::SUM);
DEFINE_timeseries(deepcopy_nfired, "deepcopy_nfired", fb303::SUM);
DEFINE_timeseries(delay_nfired, "delay_nfired", fb303::SUM);
DEFINE_timeseries(shape_nfired, "shape_nfired", fb303::SUM);
DEFINE_timeseries(client_nfired, "client_nfired", fb303::SUM);

/* A closure to initialize global counters for the number of time kernels fire
 *
 * @return  The map of counter indexed by kernel registration key
 */
std::unordered_map<std::string, int>& Kernel::get_nfired() const {
  static std::unordered_map<std::string, int> nfired;
  return nfired;
}

} // namespace facebook::cea::chips::adsim

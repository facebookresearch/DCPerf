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

#include <string>

#include <folly/MapUtil.h>
#include <folly/dynamic.h>

#include <cea/chips/adsim/cpp2/server/dwarfs/Compress.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/ConcurrentHashMap.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/DeepCopy.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Delay.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Dwarfs.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/FakeIO.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/GEMM.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/HashMap.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/IBRun.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Rebatch.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Serialize.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Shape.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/TensorDeser.h>

namespace facebook::cea::chips::adsim {

/* All kernels available to add in the config file is registed bellow */
const folly::F14VectorMap<std::string, CONFIG_F> KERNEL_DICT = {
    {"Echo", FakeIO::config},
    {"FakeIO", FakeIO::config},
    {"IBRun", IBRun::config},
    {"GEMM", GEMM::config},
    {"Compress", Compress::config},
    {"Decompress", Decompress::config},
    {"Serialize", Serialize::config},
    {"Deserialize", Deserialize::config},
    {"TensorDeser", TensorDeser::config},
    {"HashMap", HashMap::config},
    {"ConcurrentHashMap", ConcurrentHashMap::config},
    {"Rebatch", Rebatch::config},
    {"DeepCopy", DeepCopy::config},
    {"Delay", Delay::config},
    {"Shape", Shape::config},
    {"Client", Client::config},
    {"ClientLite", ClientLite::config},
};

} // namespace facebook::cea::chips::adsim

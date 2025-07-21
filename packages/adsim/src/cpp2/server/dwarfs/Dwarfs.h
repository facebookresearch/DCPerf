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

#pragma once

#include <string>

#include <folly/MapUtil.h>
#include <folly/json/dynamic.h>

#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>

namespace facebook {
namespace cea {
namespace chips {
namespace adsim {

/* The interface for a kernel to get params and config itself */
using CONFIG_F = std::shared_ptr<Kernel> (*)(const folly::dynamic&);

/* All kernels available to add in the config file is registed bellow */
extern const folly::F14VectorMap<std::string, CONFIG_F> KERNEL_DICT;

/* Struct for server to store kernel info parsed from config file */
struct KernelConfig {
  std::string name;
  std::string pool;
  int stage;
  int fanout;
  folly::dynamic params_d;
  bool deleted = false;
  KernelConfig(
      std::string name_,
      std::string pool_,
      int stage_,
      int fanout_,
      folly::dynamic params_d_)
      : name(name_),
        pool(pool_),
        stage(stage_),
        fanout(fanout_),
        params_d(params_d_) {}
};

} // namespace adsim
} // namespace chips
} // namespace cea
} // namespace facebook

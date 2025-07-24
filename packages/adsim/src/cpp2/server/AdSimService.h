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

#include <algorithm>
#include <vector>

#include <folly/Executor.h>
#include <folly/MapUtil.h>

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>
#include <cea/chips/adsim/if/gen-cpp2/AdSim.h>
#include <fb303/BaseService.h>

namespace facebook::cea::chips::adsim {

/* Structure to hold the stage information (e.g., kernels and the threadpools
 * assigned to excute the kernels) */
struct Stage {
  std::vector<std::shared_ptr<Kernel>> kernels;
  std::vector<std::shared_ptr<folly::Executor>> pools;
};

/* The main handler for AdSim service */
class AdSimHandler : virtual public AdSimSvIf, public fb303::BaseService {
 public:
  explicit AdSimHandler(
      std::shared_ptr<std::vector<Stage>> stages,
      int req_local_pool_size = 8);

  // The main endpoint to invoke AdSim service
  folly::coro::Task<std::unique_ptr<AdSimResponse>> co_sim(
      std::unique_ptr<AdSimRequest> req) override;

  // fb303 stuff
  facebook::fb303::cpp2::fb_status getStatus() override {
    return facebook::fb303::cpp2::fb_status::ALIVE;
  }
  // TODO: find out how FacebookBase2 returns the real stats
  int64_t getMemoryUsage() override {
    return 0;
  }
  double getLoad() override {
    return 0;
  }

 private:
  std::shared_ptr<AdSimHandleObjs> handle_objs;
  std::shared_ptr<std::vector<Stage>> stages;
  int num_req_local_threads;
};
} // namespace facebook::cea::chips::adsim

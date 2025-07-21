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

#include <cea/chips/adsim/cpp2/server/AdSimService.h>
#include <cea/chips/adsim/cpp2/server/DataObjects.h>

#include <fb303/ThreadCachedServiceData.h>
#include <folly/coro/Collect.h>
#include <folly/coro/FutureUtil.h>

namespace facebook::cea::chips::adsim {

// DEFINE_timeseries(request_received, "request_received", fb303::SUM);

// This line registers the counter in the system.
// DEFINE_timeseries(request_received, "request_received",
// facebook::stats::SUM);

/* AdSimHandler constructor
 *
 * @param  $stages  A pointer to pipeline stages info (constructed after thr
 *                  server parses the config file)
 * @return  AdSimHandler instance
 */
AdSimHandler::AdSimHandler(
    std::shared_ptr<std::vector<Stage>> stages,
    int req_local_pool_size)
    : fb303::BaseService("AdSim Service"),
      num_req_local_threads(req_local_pool_size) {
  this->stages = stages;
  this->handle_objs = std::make_shared<AdSimHandleObjs>();
  auto pool = std::make_shared<folly::CPUThreadPoolExecutor>(
      16, std::make_shared<folly::NamedThreadFactory>("kernelInit"));
  std::vector<folly::Future<std::string>> futures;
  for (int sid = 0; this->stages->size() > sid; ++sid) {
    int num_kernels = this->stages->at(sid).kernels.size();
    for (int i = 0; i < num_kernels; ++i) {
      // let each kernel initialize their data set
      futures.push_back(folly::via(pool.get(), [&, sid, i]() {
        return std::to_string(sid) + "K" + std::to_string(i) + " initied: " +
            this->stages->at(sid).kernels[i]->init(this->handle_objs, nullptr);
      }));
    }
  }
  auto fs = folly::collect(std::move(futures)).get();
  auto report = std::accumulate(
      fs.begin(), fs.end(), std::string(""), [](std::string a, std::string b) {
        return std::move(a) + "\n" + std::move(b);
      });
  VLOG(1) << report;
  LOG(INFO) << "All kernels initialized";
}

/* The main endpoint to invoke AdSim service
 *
 * @param  $req  A request string (content doesn't matter).
 * @return  A coroutine task
 */
folly::coro::Task<std::unique_ptr<AdSimResponse>> AdSimHandler::co_sim(
    std::unique_ptr<AdSimRequest> req) {
  auto req_pool = std::make_shared<folly::CPUThreadPoolExecutor>(
      num_req_local_threads,
      std::make_shared<folly::NamedThreadFactory>("reqLocal"));
  // STATS_request_received.add(1);
  // std::string request_str = *req->request_ref();
  std::string response_str;
  auto req_objs = std::make_shared<AdSimRequestObjs>(std::move(req));
  // Execute stage by stage
  for (const Stage& stage : *stages) {
    // Kernels in the same stage are executed by the corresponding threadpools
    // concurrantly
    std::vector<folly::coro::TaskWithExecutor<std::string>> tasks;
    for (int i = 0; i < stage.kernels.size(); ++i) {
      auto pool =
          (nullptr == stage.pools[i]) ? req_pool.get() : stage.pools[i].get();
      tasks.push_back(co_withExecutor(
          pool, stage.kernels[i]->fire(handle_objs, req_objs, stage.pools[i])));
      // stage.pools[i].get()));
    }
    auto fs = co_await folly::coro::collectAllRange(std::move(tasks));
    response_str += std::accumulate(fs.begin(), fs.end(), std::string(""));
  }
  AdSimResponse resp;
  resp.response() = response_str;
  co_return std::make_unique<AdSimResponse>(resp);
}

} // namespace facebook::cea::chips::adsim

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

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/icache_buster/ICacheBuster.h> // @manual=//cea/chips/adsim/cpp2/server/dwarfs/icache_buster:libicachebuster

#include <folly/coro/Task.h>

namespace facebook::cea::chips::adsim {

DECLARE_timeseries(ibrun_nfired);

/* A kernel for ICache-Buster */
class IBRun : public Kernel {
 public:
  explicit IBRun(int rounds, int num_methods)
      : rounds_(rounds), num_methods_(num_methods) {
    ibrun(num_methods_); // so that buster is created and methods warmed up
    // reg_nfired("IBrun");
  }
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override {
    STATS_ibrun_nfired.add(1);
    // inc_nfired("IBrun");
    // DLOG_EVERY_N(INFO, 1000) << "IBRun fired " << get_nfired()["IBrun"];
    ibrun(rounds_);
    co_return "";
  }
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    int rounds = config_d["nrounds"].asInt();
    int num_methods = config_d["nmethods"].asInt();
    return std::make_shared<IBRun>(rounds, num_methods);
  }

 private:
  int rounds_; // number of iterations
  int num_methods_; // number of functions
};
} // namespace facebook::cea::chips::adsim

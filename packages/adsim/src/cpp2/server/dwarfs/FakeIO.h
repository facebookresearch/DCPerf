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

#include <unistd.h>

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>

#include <fb303/ThreadCachedServiceData.h>
#include <folly/coro/Task.h>
#include <folly/futures/ThreadWheelTimekeeper.h>

namespace facebook {
namespace cea {
namespace chips {
namespace adsim {

DECLARE_timeseries(fakeio_nfired);

/* A simple kernel echos the request string back after a delay determined by
 * a distribution (e.g., depdent service response latency)
 */
class FakeIO : public Kernel {
  using TimekeeperPool =
      std::vector<std::shared_ptr<folly::ThreadWheelTimekeeper>>;

 public:
  /* Constructor of FakeIO kernel
   *
   * @param  $quantiles  A vector of quantiles (0~1) of interest
   * @param  $latencies  The target latency at each quantile
   * @param  $ntimekeepers  The number of timekeepers to initialize
   * @param  $input_str  A string labeling the input this kernel uses
   */
  explicit FakeIO(
      const std::vector<double>& quantiles,
      const std::vector<int64_t>& latencies,
      int ntimekeepers,
      std::string input_str)
      : ntimekeepers_(ntimekeepers), input_str_(input_str) {
    std::copy(
        quantiles.begin(), quantiles.end(), std::back_inserter(quantiles_));
    std::copy(
        latencies.begin(), latencies.end(), std::back_inserter(latencies_));
    if (quantiles_.size() != latencies_.size()) {
      LOG(ERROR) << "#quantiles (" << quantiles_.size() << ") != #latencies ("
                 << latencies_.size() << ")";
      nquantiles = (quantiles_.size() < latencies_.size()) ? quantiles_.size()
                                                           : latencies_.size();
    } else {
      nquantiles = quantiles_.size();
    }
    std::sort(quantiles_.begin(), quantiles_.begin() + nquantiles);
    std::sort(latencies_.begin(), latencies_.begin() + nquantiles);
    nfired = 0;
    // reg_nfired("FakeIO");
  }

  /* Initialize sleep time array and timekeepers for the FakeIO kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects
   */
  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override {
    Kernel::init(h_objs, r_objs);
    auto timekeepers = std::make_shared<TimekeeperPool>();
    for (int i = 0; ntimekeepers_ > i; ++i) {
      timekeepers->push_back(std::make_shared<folly::ThreadWheelTimekeeper>());
    }
    h_objs->set_shared_ptr(input_str_ + ".timekeepers", timekeepers);

    void* lat_arr = malloc(QUANTILE_LEN() * sizeof(int64_t));
    int64_t* lat_arr_i64 = (int64_t*)lat_arr;
    fit_quantile(
        quantiles_, nquantiles, latencies_, lat_arr_i64, QUANTILE_LEN());
    h_objs->set_plain_ptr(input_str_ + ".lat_arr", lat_arr);
    return folly::to<std::string>(
        "FakeIO: ",
        input_str_,
        ".lat_arr ",
        std::to_string(QUANTILE_LEN() * sizeof(int64_t)),
        "B");
  }

  /* Invoke FakeIO kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects
   * @return  A Delay coroutine task
   */
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override {
    STATS_fakeio_nfired.add(1);
    auto timekeepers =
        h_objs->get_shared_ptr<TimekeeperPool>(input_str_ + ".timekeepers");
    int64_t* lat_arr = (int64_t*)h_objs->get_plain_ptr(input_str_ + ".lat_arr");
    int64_t* idx = (int64_t*)r_objs->get_plain_ptr_default("multiplier", [&]() {
      void* i = malloc(sizeof(int64_t));
      int64_t* i64 = (int64_t*)i;
      *i64 = get_rand() % QUANTILE_LEN();
      return i;
    });
    int64_t us2sleep = lat_arr[*idx];
    co_await folly::futures::sleep(
        std::chrono::microseconds{us2sleep},
        timekeepers->at(nfired++ % timekeepers->size()).get());
    // NOTE: might be race condition on nfired, but safe as it is bounded by %
    co_return "";
  }

  /* Builder of the FakeIO kernel, create an instance according to the config
   *
   * @param  $config_d  Dynamic container of configuraiton
   * @return  A configured FakeIO kernel
   */
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    std::vector<double> quantiles;
    std::vector<int64_t> latencies;
    int ntimekeepers = 4;
    std::string input_str = "fakeio";
    if (0 == config_d.count("quantiles") || !config_d["quantiles"].isArray()) {
      LOG(ERROR) << "quantiles needs to be an array of double";
    } else {
      for (auto q_d : config_d["quantiles"]) {
        quantiles.push_back(q_d.asDouble() * QUANTILE_LEN());
      }
    }
    if (0 == config_d.count("latencies") || !config_d["latencies"].isArray()) {
      LOG(ERROR) << "latencies needs to be an array of int (in ms)";
    } else {
      for (auto l_d : config_d["latencies"]) {
        latencies.push_back(l_d.asInt());
      }
    }
    if (0 != config_d.count("ntimekeepers")) {
      ntimekeepers = config_d["ntimekeepers"].asInt();
    }
    if (0 != config_d.count("input")) {
      input_str = config_d["input"].asString();
    }
    return std::make_shared<FakeIO>(
        quantiles, latencies, ntimekeepers, input_str);
  }

 private:
  static constexpr int QUANTILE_LEN() {
    return 100000;
  } // differentiate up to 0.001%
  std::vector<double> quantiles_; // latency in us
  std::vector<int64_t> latencies_; // latency in us
  int nquantiles;
  int ntimekeepers_;
  std::string input_str_;
  int nfired;
};
} // namespace adsim
} // namespace chips
} // namespace cea
} // namespace facebook

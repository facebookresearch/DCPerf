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
#include <folly/stats/QuantileEstimator.h>
#include <sys/time.h>

namespace facebook {
namespace cea {
namespace chips {
namespace adsim {

DECLARE_timeseries(delay_nfired);

/* A kernel adjust latency to meet percentile target by control the delay.
 * Note: this should be the last kernel in the pipeline */
class Delay : public Kernel {
  using TimekeeperPool =
      std::vector<std::shared_ptr<folly::ThreadWheelTimekeeper>>;

 public:
  /* Constructor of Delay kernel
   *
   * @param  $quantiles  A vector of quantiles (0~1) of interest
   * @param  $latencies  The target latency at each quantile
   * @param  $ntimekeepers  The number of timekeepers to initialize
   * @param  $input_str  A string labeling the input this kernel uses
   */
  explicit Delay(
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
  }

  /* Initialize latency histogram and timekeepers for the Delay kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects
   */
  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> /*unused*/) override {
    auto lat_hist = std::make_shared<folly::SimpleQuantileEstimator<>>();
    h_objs->set_shared_ptr(input_str_ + ".lat_hist", lat_hist);
    auto timekeepers = std::make_shared<TimekeeperPool>();
    for (int i = 0; ntimekeepers_ > i; ++i) {
      timekeepers->push_back(std::make_shared<folly::ThreadWheelTimekeeper>());
    }
    h_objs->set_shared_ptr(input_str_ + ".timekeepers", timekeepers);
    return "Delay";
  }

  /* Invoke Delay kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects
   * @return  A Delay coroutine task
   */
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override {
    STATS_delay_nfired.add(1);
    auto lat_hist = h_objs->get_shared_ptr<folly::SimpleQuantileEstimator<>>(
        input_str_ + ".lat_hist");
    auto timekeepers =
        h_objs->get_shared_ptr<TimekeeperPool>(input_str_ + ".timekeepers");
    auto est = lat_hist->estimateQuantiles(quantiles_);
    int64_t beg = r_objs->receive_us();
    int64_t now = now_us();
    int64_t lat = now - beg + 10;
    for (int i = 0; nquantiles > i; ++i) {
      if (lat > latencies_[i]) { // cannot make it shorter
        continue;
      } else if (
          latencies_[i] >= est.quantiles[i].second && (nquantiles - 1) != i) {
        // case 1: actual quantile fits the target well, then go ahead
        // checking whether other bigger quantiles need adjustment
        // case 2: actual quantile is shorter, then a longer delay
        // would help to shift the distribution to close this quantile
        // case 3: reach the last quantile, all quantiles before are equal
        // or smaller, then let's do the longest delay anyway
        continue;
      } else {
        int64_t us2sleep = latencies_[i] - lat;
        lat_hist->addValue(latencies_[i]); // Note: potential thread contention
        co_await folly::futures::sleep(
            std::chrono::microseconds{us2sleep},
            timekeepers->at(((int)est.count) % timekeepers->size()).get());
        break;
      }
    }
    co_return "";
  }

  /* Builder of the Delay kernel, create an instance according to the config
   *
   * @param  $config_d  Dynamic container of configuraiton
   * @return  A configured Delay kernel
   */
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    std::vector<double> quantiles;
    std::vector<int64_t> latencies;
    int ntimekeepers = 4;
    std::string input_str = "delay";
    if (0 == config_d.count("quantiles") || !config_d["quantiles"].isArray()) {
      LOG(ERROR) << "quantiles needs to be an array of double";
    } else {
      for (auto q_d : config_d["quantiles"]) {
        quantiles.push_back(q_d.asDouble());
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
    return std::make_shared<Delay>(
        quantiles, latencies, ntimekeepers, input_str);
  }

 private:
  /* Helper function to get current time in us
   *
   * @return  Current time in us
   */
  int64_t now_us() {
    struct timespec ts;
    int r = clock_gettime(CLOCK_MONOTONIC, &ts);
    PCHECK(0 == r);
    return (ts.tv_nsec / 1000 + ts.tv_sec * 1000000);
  }
  std::vector<double> quantiles_; // latency in us
  std::vector<int64_t> latencies_; // latency in us
  int nquantiles;
  int ntimekeepers_;
  std::string input_str_;
};
} // namespace adsim
} // namespace chips
} // namespace cea
} // namespace facebook

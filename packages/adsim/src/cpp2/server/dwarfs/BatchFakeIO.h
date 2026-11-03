/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
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

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/BatchFakeIOImpl.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>

#include <folly/Conv.h>
#include <folly/coro/Task.h>
#include <folly/dynamic.h>
#include <folly/futures/SharedPromise.h>
#include <folly/futures/ThreadWheelTimekeeper.h>

namespace facebook::cea::chips::adsim {

/* A kernel that models a dependency serving batched requests, such as a GPU
 * predictor: requests that arrive within window_us of a batch's first request
 * form one batch, and every request in it completes when the batch does,
 * (base_us + per_request_us * n^exponent) times lognormal noise after the
 * window closes, n being the batch size. It sleeps on timekeepers and uses no
 * CPU, so its latency grows with load the way a batching dependency's does.
 */
class BatchFakeIO : public Kernel {
 public:
  /* Constructor of BatchFakeIO kernel
   *
   * @param  $params  Window, batch-time and noise parameters
   * @param  $ntimekeepers  Number of timekeepers to sleep on
   * @param  $seed  Seed of the noise generator
   */
  BatchFakeIO(
      const batch_fake_io::Params& params,
      int ntimekeepers,
      uint64_t seed)
      : params_{params},
        ntimekeepers_{ntimekeepers},
        noise_{params.sigma, seed} {
    batch_fake_io::validate(params_);
    if (0 >= ntimekeepers_) {
      throw std::invalid_argument("BatchFakeIO: ntimekeepers must be positive");
    }
  }

  std::string init(
      std::shared_ptr<AdSimHandleObjs> /*unused*/,
      std::shared_ptr<AdSimRequestObjs> /*unused*/) override {
    for (int i = 0; ntimekeepers_ > i; ++i) {
      timekeepers_.push_back(std::make_shared<folly::ThreadWheelTimekeeper>());
    }
    return "BatchFakeIO";
  }

  /* Invoke BatchFakeIO kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects
   * @return  A coroutine task that completes with the request's batch
   */
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> /*unused*/,
      std::shared_ptr<AdSimRequestObjs> /*unused*/,
      std::shared_ptr<folly::Executor> /*unused*/) override {
    auto [batch, opened] = batcher_.join();
    if (!opened) {
      co_await batch->done.getSemiFuture();
      co_return "";
    }
    auto* timekeeper = timekeepers_.at(nfired_++ % timekeepers_.size()).get();
    co_await folly::futures::sleep(usec(params_.window_us), timekeeper);
    const int64_t size = batcher_.close(batch);
    co_await folly::futures::sleep(
        usec(batch_fake_io::batchTimeUs(params_, size) * noise_.sample()),
        timekeeper);
    batch->done.setValue();
    co_return "";
  }

  /* Builder of the BatchFakeIO kernel, create an instance according to the
   * config
   *
   * @param  $config_d  Dynamic container of configuration
   * @return  A configured BatchFakeIO kernel
   */
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    const batch_fake_io::Params params{
        .window_us = config_d.getDefault("window_us", 22000.0).asDouble(),
        .base_us = config_d.getDefault("base_us", 0.0).asDouble(),
        .per_request_us = config_d.getDefault("per_request_us", 0.0).asDouble(),
        .exponent = config_d.getDefault("exponent", 1.0).asDouble(),
        .sigma = config_d.getDefault("sigma", 0.0).asDouble(),
    };
    const uint64_t seed = config_d.count("seed")
        ? folly::to<uint64_t>(config_d["seed"].asInt())
        : std::random_device{}();
    return std::make_shared<BatchFakeIO>(
        params,
        folly::to<int>(config_d.getDefault("ntimekeepers", 4).asInt()),
        seed);
  }

 private:
  struct Batch {
    int64_t size = 0;
    folly::SharedPromise<folly::Unit> done;
  };

  static std::chrono::microseconds usec(double us) {
    return std::chrono::microseconds{std::llround(us)};
  }

  batch_fake_io::Params params_;
  int ntimekeepers_;
  batch_fake_io::Noise noise_;
  batch_fake_io::Batcher<Batch> batcher_;
  std::vector<std::shared_ptr<folly::ThreadWheelTimekeeper>> timekeepers_;
  std::atomic<uint64_t> nfired_{0};
};

} // namespace facebook::cea::chips::adsim

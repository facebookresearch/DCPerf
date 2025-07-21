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
#include <numeric>
#include <vector>

#include <assert.h>

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>

#include <fb303/ThreadCachedServiceData.h>
#include <folly/coro/Collect.h>
#include <folly/coro/Task.h>

namespace facebook {
namespace cea {
namespace chips {
namespace adsim {

DECLARE_timeseries(gemm_nfired);

/* A kernel for GEMM */
class GEMM : public Kernel {
 public:
  explicit GEMM(
      int M,
      int N,
      int K,
      int niters,
      int nthreads,
      std::string input_str)
      : M_(M),
        N_(N),
        K_(K),
        niters_(niters),
        nthreads_(nthreads),
        input_str_(input_str) {
    // reg_nfired("GEMM");
  }
  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override;
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override {
    STATS_gemm_nfired.add(1);
    // inc_nfired("GEMM");
    // DLOG_EVERY_N(INFO, 1000) << "GEMM fired " << get_nfired()["GEMM"];
    if (0 == nthreads_) {
      performance_test(niters_, h_objs);
    } else {
      std::vector<folly::coro::TaskWithExecutor<int>> tasks;
      for (int i = 0; i < nthreads_; ++i) {
        tasks.push_back(
            co_withExecutor(pool.get(), co_performance_test(niters_, h_objs)));
      }
      auto fs = co_await folly::coro::collectAllRange(std::move(tasks));
      [[maybe_unused]] auto res = std::accumulate(fs.begin(), fs.end(), 0);
      assert(res == nthreads_);
    }
    co_return "";
  }
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    int M = config_d["M"].asInt();
    int N = config_d["N"].asInt();
    int K = config_d["K"].asInt();
    int niters = 1;
    int nthreads = 0;
    std::string input_str = "Hfbgemm";
    if (config_d.count("niters")) {
      niters = config_d["niters"].asInt();
    }
    if (config_d.count("nthreads")) {
      nthreads = config_d["nthreads"].asInt();
    }
    if (config_d.count("input")) {
      input_str = config_d["input"].asString();
    }
    return std::make_shared<GEMM>(M, N, K, niters, nthreads, input_str);
  }

 private:
  void performance_test(
      const int niter,
      std::shared_ptr<AdSimHandleObjs> h_objs);
  folly::coro::Task<int> co_performance_test(
      const int niters,
      std::shared_ptr<AdSimHandleObjs> h_objs) {
    performance_test(niters, h_objs);
    co_return 1;
  }
  int M_, N_, K_;
  int niters_;
  int nthreads_;
  std::string input_str_;
};

} // namespace adsim
} // namespace chips
} // namespace cea
} // namespace facebook

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
#include <string>
#include <unordered_map>
#include <vector>

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <folly/coro/Task.h>
#include <folly/json/dynamic.h>
#include <glog/logging.h>

namespace facebook {
namespace cea {
namespace chips {
namespace adsim {

/* Base class of kernels */
class Kernel {
 public:
  /* Kernel constructor that initializes an array of random number.
   *
   * @params  $rand_max
   */
  explicit Kernel(int64_t rand_max = 262144) : rand_max_(rand_max) {}
  virtual std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> /* unused */) {
    rand_arr = (int64_t*)h_objs->get_plain_ptr_default(
        "rand_arr", [rand_max_ = this->rand_max_]() {
          int64_t* rand_arr_2 = new int64_t[rand_max_];
          std::vector<int64_t> rand_tmp(rand_max_);
          int64_t i = 0;
          std::generate_n(rand_tmp.begin(), rand_max_, [&i]() { return i++; });
          std::random_shuffle(rand_tmp.begin(), rand_tmp.end());
          for (i = 0; rand_max_ > i; ++i) {
            rand_arr_2[rand_tmp[i]] = rand_tmp[(i + 1) % rand_max_];
          }
          return (void*)rand_arr_2;
        });
    rand_idx = 0;
    return "";
  }

  virtual folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs>,
      std::shared_ptr<AdSimRequestObjs>,
      std::shared_ptr<folly::Executor>) = 0;
  virtual ~Kernel() {}
  void dump_nfired() const {
    for (auto iter : get_nfired()) {
      LOG(INFO) << iter.first << ": " << iter.second;
    }
  }

 protected:
  std::unordered_map<std::string, int>& get_nfired() const;
  void reg_nfired(const std::string& name) {
    get_nfired()[name] = 0;
  }
  void inc_nfired(const std::string& name) {
    get_nfired()[name] += 1;
  }
  int64_t rand_max_ = 262144;
  int64_t rand_idx = 0;
  int64_t* rand_arr = nullptr;
  int64_t get_rand() {
    rand_idx = rand_arr[rand_idx];
    return rand_idx;
  }
  // NOTE: there might be race condition, but getting random number doesn't
  // need to be thread-safe
  void fit_quantile(
      const std::vector<double>& quantiles,
      int nquantiles,
      const std::vector<int64_t>& tgts,
      int64_t* arr,
      int arr_len) {
    for (int i = 0; quantiles[0] > i; ++i) {
      arr[i] = tgts[0] * (i / quantiles[0]);
      // LOG(INFO) << i << ", min-0: " << arr[i];
    }
    int idx = 0;
    uint64_t qdiff = (1 > quantiles[0]) ? 1 : quantiles[0];
    int64_t tdiff = tgts[0];
    for (uint64_t i = quantiles[0]; quantiles[nquantiles - 1] > i; ++i) {
      if (quantiles[idx] <= i) {
        idx++;
        qdiff = quantiles[idx] - quantiles[idx - 1];
        qdiff = (0 == qdiff) ? 1 : qdiff;
        tdiff = tgts[idx] - tgts[idx - 1];
      }
      uint64_t offset = i - quantiles[idx - 1];
      arr[i] = tgts[idx - 1] + tdiff * offset / qdiff;
      // LOG(INFO) << i << ", " << (idx - 1) << "-" << idx << ": " << arr[i];
    }
    for (int i = quantiles[nquantiles - 1]; arr_len > i; ++i) {
      arr[i] = tgts[nquantiles - 1];
      // LOG(INFO) << i << ", " << idx << "-max: " << arr[i];
    }
  }
  std::string gen_string(const int len) {
    std::string tmp;
    static const char alphabet[] =
        "0123456789 ,.!:'\"@#$%&*"
        "ABCEDFGHIJKLMNOPQRSTUVWXYZabcedfghijklmnopqrstuvwxyz";
    std::random_device rd;
    std::default_random_engine rng(rd());
    std::uniform_int_distribution<> dist(
        0, sizeof(alphabet) / sizeof(*alphabet) - 2);
    tmp.reserve(len);
    std::generate_n(
        std::back_inserter(tmp), len, [&]() { return alphabet[dist(rng)]; });
    return tmp;
  }
};

} // namespace adsim
} // namespace chips
} // namespace cea
} // namespace facebook

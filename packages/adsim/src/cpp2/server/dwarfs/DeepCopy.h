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

#include <algorithm>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>

#include <fb303/ThreadCachedServiceData.h>
#include <folly/container/F14Map.h>
#include <folly/coro/Task.h>
#include <folly/sorted_vector_types.h>

namespace facebook::cea::chips::adsim {

DECLARE_timeseries(deepcopy_nfired);

struct CopyUnit {
  std::vector<int64_t> i64_vec;
  std::map<std::string, int64_t> stri64_map;
  std::unordered_map<std::string, int64_t> stri64_hashtab;
};

/* A kernel does deep copy operations intensively */
class DeepCopy : public Kernel {
 public:
  explicit DeepCopy(
      int copy_len,
      int unit_vec_len,
      int i64_vec_len,
      int stri64_map_size,
      int stri64_hashtab_size,
      std::string input_str,
      std::string output_str)
      : copy_len_(copy_len),
        UNIT_VEC_LEN(unit_vec_len),
        I64_VEC_LEN(i64_vec_len),
        STRI64_MAP_SIZE(stri64_map_size),
        STRI64_HASHTAB_SIZE(stri64_hashtab_size),
        input_str_(input_str),
        output_str_(output_str) {}

  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override {
    Kernel::init(h_objs, r_objs);
    if (0 < input_str_.size() && 'H' == input_str_[0]) {
      auto unit_vec = std::make_shared<std::vector<CopyUnit>>(UNIT_VEC_LEN);
      for (auto& unit : *unit_vec.get()) {
        unit.i64_vec.resize(I64_VEC_LEN);
#pragma omp parallel for num_threads(10)
        for (int i = 0; I64_VEC_LEN > i; ++i) {
          unit.i64_vec.at(i) = get_rand();
        }
        const int kSalt = 0x619abc7e;
        for (int i = 0; STRI64_MAP_SIZE > i; ++i) {
          std::string key =
              folly::to<std::string>(folly::hash::jenkins_rev_mix32(i ^ kSalt));
          unit.stri64_map.insert(std::make_pair(key, get_rand()));
        }
        unit.stri64_hashtab.reserve(STRI64_HASHTAB_SIZE);
        for (int i = 0; STRI64_HASHTAB_SIZE > i; ++i) {
          std::string key =
              folly::to<std::string>(folly::hash::jenkins_rev_mix32(i ^ kSalt));
          unit.stri64_hashtab.insert(std::make_pair(key, get_rand()));
        }
      }
      h_objs->set_shared_ptr(input_str_, unit_vec);
      size_t total_size = UNIT_VEC_LEN *
          (sizeof(int64_t) * I64_VEC_LEN + 8 * STRI64_MAP_SIZE +
           8 * STRI64_HASHTAB_SIZE);
      return folly::to<std::string>(
          "DeepCopy: ",
          input_str_,
          " ",
          std::to_string(UNIT_VEC_LEN),
          "x(8x",
          std::to_string(I64_VEC_LEN),
          "+8x",
          std::to_string(STRI64_MAP_SIZE),
          "+8x",
          std::to_string(STRI64_HASHTAB_SIZE),
          ")=",
          std::to_string(total_size),
          "B");
    } else {
      return "DeepCopy: " + input_str_;
    }
  }

  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override {
    STATS_deepcopy_nfired.add(1);
    int idx = get_rand();
    std::shared_ptr<std::vector<CopyUnit>> unit_vec;
    int index_max;
    if (0 < input_str_.size() && 'H' == input_str_[0]) {
      unit_vec = h_objs->get_shared_ptr<std::vector<CopyUnit>>(input_str_);
      index_max = UNIT_VEC_LEN;
    } else {
      unit_vec = r_objs->get_shared_ptr<std::vector<CopyUnit>>(input_str_);
      index_max = unit_vec->size();
    }
    auto dest_vec = std::make_shared<std::vector<CopyUnit>>(copy_len_);
    for (int i = 0; copy_len_ > i; ++i) {
      int index = (i + idx) % index_max;
      dest_vec->at(i).i64_vec.reserve(unit_vec->at(index).i64_vec.size());
      dest_vec->at(i).stri64_hashtab.reserve(
          unit_vec->at(index).stri64_hashtab.size());
      std::copy(
          unit_vec->at(index).i64_vec.begin(),
          unit_vec->at(index).i64_vec.end(),
          std::back_inserter(dest_vec->at(i).i64_vec));
      std::copy(
          unit_vec->at(index).stri64_map.begin(),
          unit_vec->at(index).stri64_map.end(),
          std::inserter(
              dest_vec->at(i).stri64_map, dest_vec->at(i).stri64_map.begin()));
      std::copy(
          unit_vec->at(index).stri64_hashtab.begin(),
          unit_vec->at(index).stri64_hashtab.end(),
          std::inserter(
              dest_vec->at(i).stri64_hashtab,
              dest_vec->at(i).stri64_hashtab.begin()));
    }
    r_objs->set_shared_ptr(output_str_, dest_vec);
    co_return "";
  }

  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    int copy_len = config_d["copy_len"].asInt();
    int unit_vec_len = 1024;
    int i64_vec_len = 64;
    int stri64_map_size = 0;
    int stri64_hashtab_size = 0;
    std::string input_str = "Hdeepcopy";
    std::string output_str = "Rdeepcopy";
    if (config_d.count("unit_vec_len")) {
      unit_vec_len = config_d["unit_vec_len"].asInt();
    }
    if (config_d.count("i64_vec_len")) {
      i64_vec_len = config_d["i64_vec_len"].asInt();
    }
    if (config_d.count("stri64_map_size")) {
      stri64_map_size = config_d["stri64_map_size"].asInt();
    }
    if (config_d.count("stri64_hashtab_size")) {
      stri64_hashtab_size = config_d["stri64_hashtab_size"].asInt();
    }
    if (config_d.count("input")) {
      input_str = config_d["input"].asString();
    }
    if (config_d.count("output")) {
      output_str = config_d["output"].asString();
    }
    return std::make_shared<DeepCopy>(
        copy_len,
        unit_vec_len,
        i64_vec_len,
        stri64_map_size,
        stri64_hashtab_size,
        input_str,
        output_str);
  }

 private:
  int copy_len_;
  int UNIT_VEC_LEN;
  int I64_VEC_LEN;
  int STRI64_MAP_SIZE;
  int STRI64_HASHTAB_SIZE;
  std::string input_str_;
  std::string output_str_;
};
} // namespace facebook::cea::chips::adsim

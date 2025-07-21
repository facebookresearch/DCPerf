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

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>

#include <folly/coro/Task.h>

#include <thrift/lib/cpp2/protocol/CompactProtocol.h>
#include <thrift/lib/cpp2/protocol/Serializer.h>

#include <cea/chips/adsim/if/gen-cpp2/Serialize_types.h>

namespace facebook {
namespace cea {
namespace chips {
namespace adsim {

DECLARE_timeseries(serialize_nfired);
DECLARE_timeseries(deserialize_nfired);

/* A base class for serialize/deserialize kernels */
class SerializeBase : public Kernel {
 public:
  explicit SerializeBase(
      int req_vec_len,
      int req_unit_list_len,
      int unit_list_map_size,
      int unit_info_list_len,
      int unit_i32_list_len,
      int niters,
      std::string input_str,
      std::string output_str)
      : Kernel(),
        REQ_VEC_LEN(req_vec_len),
        REQ_UNIT_LIST_LEN(req_unit_list_len),
        UNIT_LIST_MAP_SIZE(unit_list_map_size),
        UNIT_INFO_LIST_LEN(unit_info_list_len),
        UNIT_I32_LIST_LEN(unit_i32_list_len),
        niters_(niters),
        input_str_(input_str),
        output_str_(output_str) {}
  explicit SerializeBase(const SerializeBase& rhs) : Kernel() {
    this->REQ_VEC_LEN = rhs.REQ_VEC_LEN;
    this->REQ_UNIT_LIST_LEN = rhs.REQ_UNIT_LIST_LEN;
    this->UNIT_LIST_MAP_SIZE = rhs.UNIT_LIST_MAP_SIZE;
    this->UNIT_INFO_LIST_LEN = rhs.UNIT_INFO_LIST_LEN;
    this->UNIT_I32_LIST_LEN = rhs.UNIT_I32_LIST_LEN;
    this->niters_ = rhs.niters_;
    this->input_str_ = rhs.input_str_;
    this->output_str_ = rhs.output_str_;
  }
  SerializeBase& operator=(const SerializeBase& rhs) {
    this->REQ_VEC_LEN = rhs.REQ_VEC_LEN;
    this->REQ_UNIT_LIST_LEN = rhs.REQ_UNIT_LIST_LEN;
    this->UNIT_LIST_MAP_SIZE = rhs.UNIT_LIST_MAP_SIZE;
    this->UNIT_INFO_LIST_LEN = rhs.UNIT_INFO_LIST_LEN;
    this->UNIT_I32_LIST_LEN = rhs.UNIT_I32_LIST_LEN;
    this->niters_ = rhs.niters_;
    this->input_str_ = rhs.input_str_;
    this->output_str_ = rhs.output_str_;
    return *this;
  }
  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override {
    Kernel::init(h_objs, r_objs);
    if (0 == input_str_.size() || 'H' != input_str_[0]) {
      return input_str_;
    }
    // pre-populate units
    std::vector<SerializableUnit> units(REQ_VEC_LEN);
    for (auto& unit : units) {
      unit.list_of_info()->resize(UNIT_INFO_LIST_LEN);
      std::for_each(
          unit.list_of_info()->begin(),
          unit.list_of_info()->end(),
          [&](auto& info) {
            info.var_bool() = (0 == (get_rand() % 2));
            info.var_i32() = get_rand();
            info.var_double() = get_rand() * 3.14;
          });
      unit.list_of_i32()->resize(UNIT_I32_LIST_LEN);
      std::generate(
          unit.list_of_i32()->begin(), unit.list_of_i32()->end(), [&]() {
            return get_rand();
          });
      unit.map_of_list()->reserve(UNIT_LIST_MAP_SIZE);
      for (int i = 0; UNIT_LIST_MAP_SIZE > i; ++i) {
        id_list_t tmp(UNIT_I32_LIST_LEN);
        std::generate(tmp.begin(), tmp.end(), [&]() { return get_rand(); });
        unit.map_of_list()->insert(
            std::make_pair<int32_t, id_list_t>(get_rand(), std::move(tmp)));
      }
    }
    size_t unit_size =
        (UNIT_INFO_LIST_LEN * sizeof(SerializableInfo) +
         UNIT_I32_LIST_LEN * sizeof(int32_t) +
         UNIT_LIST_MAP_SIZE *
             (sizeof(int32_t) + UNIT_I32_LIST_LEN * sizeof(int32_t)));
    // pre-populate requests
    auto reqs = std::make_shared<std::vector<SerializableReq>>(REQ_VEC_LEN);
    for (auto& req : *reqs.get()) {
      req.var_i64() = get_rand();
      req.list_of_unit()->resize(REQ_UNIT_LIST_LEN);
#pragma omp parallel for num_threads(10)
      for (int i = 0; REQ_UNIT_LIST_LEN > i; ++i) {
        req.list_of_unit()->at(i) = units[get_rand() % REQ_VEC_LEN];
      }
    }
    size_t total_size =
        (sizeof(int64_t) + REQ_UNIT_LIST_LEN * unit_size) * REQ_VEC_LEN;
    // LOG(INFO) << "size per SerializableUnit = " << unit_size <<
    //   ", size per SerializableReq = " <<
    //   (sizeof(int64_t) + REQ_UNIT_LIST_LEN * unit_size);
    //  save the requests to handler objects
    h_objs->set_shared_ptr(input_str_, reqs);
    return folly::to<std::string>(
        input_str_,
        " ",
        std::to_string(REQ_VEC_LEN),
        "x(8+",
        std::to_string(REQ_UNIT_LIST_LEN),
        "x",
        std::to_string(unit_size),
        ")=",
        std::to_string(total_size),
        "B");
  }
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs>,
      std::shared_ptr<AdSimRequestObjs>,
      std::shared_ptr<folly::Executor>) override {
    co_return "";
  }
  static SerializeBase config(
      const folly::dynamic& config_d,
      std::string input_str,
      std::string output_str) {
    int req_vec_len = 512;
    int req_unit_list_len = 10;
    int unit_list_map_size = 19;
    int unit_info_list_len = 23;
    int unit_i32_list_len = 17;
    int niters = 1;
    if (config_d.count("req_vec_len")) {
      req_vec_len = config_d["req_vec_len"].asInt();
    }
    if (config_d.count("req_unit_list_len")) {
      req_unit_list_len = config_d["req_unit_list_len"].asInt();
    }
    if (config_d.count("unit_list_map_size")) {
      unit_list_map_size = config_d["unit_list_map_size"].asInt();
    }
    if (config_d.count("unit_info_list_len")) {
      unit_info_list_len = config_d["unit_info_list_len"].asInt();
    }
    if (config_d.count("unit_i32_list_len")) {
      unit_i32_list_len = config_d["unit_i32_list_len"].asInt();
    }
    if (config_d.count("niters")) {
      niters = config_d["niters"].asInt();
    }
    if (config_d.count("input")) {
      input_str = config_d["input"].asString();
    }
    if (config_d.count("output")) {
      output_str = config_d["output"].asString();
    }
    return SerializeBase(
        req_vec_len,
        req_unit_list_len,
        unit_list_map_size,
        unit_info_list_len,
        unit_i32_list_len,
        niters,
        input_str,
        output_str);
  }

 protected:
  int REQ_VEC_LEN;
  int REQ_UNIT_LIST_LEN;
  int UNIT_LIST_MAP_SIZE;
  int UNIT_INFO_LIST_LEN;
  int UNIT_I32_LIST_LEN;
  int niters_;
  std::string input_str_;
  std::string output_str_;
};

/* A kernel does Serialization using fbthrift */
class Serialize : public SerializeBase {
 public:
  explicit Serialize(const SerializeBase& base) : SerializeBase(base) {}
  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override {
    return "Serialize: " + SerializeBase::init(h_objs, r_objs);
  }
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override {
    STATS_serialize_nfired.add(1);
    auto iobufq = std::make_shared<folly::IOBufQueue>();
    if (0 < input_str_.size() && 'H' == input_str_[0]) {
      auto serializable_reqs =
          h_objs->get_shared_ptr<std::vector<SerializableReq>>(input_str_);
      for (int i = 0; niters_ > i; ++i) {
        int idx = get_rand() % REQ_VEC_LEN;
        apache::thrift::CompactSerializer::serialize(
            serializable_reqs->at(idx), iobufq.get());
      }
    } else {
      auto serializable_req =
          r_objs->get_shared_ptr<SerializableReq>(input_str_);
      for (int i = 0; niters_ > i; ++i) {
        apache::thrift::CompactSerializer::serialize(
            *serializable_req.get(), iobufq.get());
      }
    }
    r_objs->set_shared_ptr(output_str_, iobufq);
    co_return "";
  }
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    return std::make_shared<Serialize>(SerializeBase::config(
        config_d, "Hserializable_req_list", "Rserialized_req"));
  }
};

///* A kernel does Deserialization using fbthrift */
class Deserialize : public SerializeBase {
 public:
  explicit Deserialize(const SerializeBase& base) : SerializeBase(base) {}
  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override {
    if (0 == input_str_.size() || 'H' != input_str_[0]) {
      return "Deserialize: " + input_str_;
    }
    auto report = SerializeBase::init(h_objs, r_objs);
    // get the serialized reqs
    auto iobufq_vec = std::make_shared<std::vector<folly::IOBufQueue>>();
    auto reqs =
        h_objs->get_shared_ptr<std::vector<SerializableReq>>(input_str_);
    for (const auto& req : *reqs.get()) {
      iobufq_vec->emplace_back();
      apache::thrift::CompactSerializer::serialize(req, &iobufq_vec->back());
    }
    h_objs->set_shared_ptr<std::vector<folly::IOBufQueue>>(
        input_str_ + ".iobufq_vec", iobufq_vec);
    return folly::to<std::string>(
        "Deserialize: ", report, " ", input_str_, ".iobufq_vec");
  }
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override {
    STATS_deserialize_nfired.add(1);
    auto deserialized = std::make_shared<SerializableReq>();
    if (0 < input_str_.size() && 'H' == input_str_[0]) {
      int idx = get_rand() % REQ_VEC_LEN;
      auto iobufq_vec = h_objs->get_shared_ptr<std::vector<folly::IOBufQueue>>(
          input_str_ + ".iobufq_vec");
      for (int i = 0; niters_ > i; ++i) {
        apache::thrift::CompactSerializer::deserialize(
            iobufq_vec->at(idx).front(), *deserialized.get());
      }
    } else {
      auto iobufq = r_objs->get_shared_ptr<folly::IOBufQueue>(input_str_);
      for (int i = 0; niters_ > i; ++i) {
        apache::thrift::CompactSerializer::deserialize(
            iobufq->front(), *deserialized.get());
      }
    }
    r_objs->set_shared_ptr(output_str_, deserialized);
    co_return "";
  }
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    return std::make_shared<Deserialize>(
        SerializeBase::config(config_d, "Hdeserialize", "Rdeserialized"));
  }
};
} // namespace adsim
} // namespace chips
} // namespace cea
} // namespace facebook

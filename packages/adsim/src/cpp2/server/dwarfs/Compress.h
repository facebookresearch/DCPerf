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

#include <lzbench.h>
#include <util.h>

#include <string.h>

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>

#include <folly/coro/Task.h>

namespace facebook::cea::chips::adsim {

/* A base kernel for compress/decompress kernel */
class CompressBase : public Kernel {
 public:
  explicit CompressBase(
      std::string encoder,
      int level,
      size_t size,
      std::string input_str,
      std::string output_str);
  explicit CompressBase(const CompressBase& rhs) : Kernel() {
    this->compressor_ = rhs.compressor_;
    this->level_ = rhs.level_;
    this->size_ = rhs.size_;
    this->compressed_size_ = rhs.compressed_size_;
    this->input_str_ = rhs.input_str_;
    this->output_str_ = rhs.output_str_;
  }
  CompressBase& operator=(const CompressBase& rhs) {
    this->compressor_ = rhs.compressor_;
    this->level_ = rhs.level_;
    this->size_ = rhs.size_;
    this->compressed_size_ = rhs.compressed_size_;
    this->input_str_ = rhs.input_str_;
    this->output_str_ = rhs.output_str_;
    return *this;
  }
  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override;
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override {
    co_return "";
  }
  static CompressBase config(
      const folly::dynamic& config_d,
      std::string input_str,
      std::string output_str) {
    std::string encoder = config_d["compressor"].asString();
    int level = config_d["level"].asInt();
    size_t size = config_d["size"].asInt();
    if (config_d.count("input")) {
      input_str = config_d["input"].asString();
    }
    if (config_d.count("output")) {
      output_str = config_d["output"].asString();
    }
    return CompressBase(encoder, level, size, input_str, output_str);
  }

 protected:
  const compressor_desc_t* compressor_ = nullptr;
  int level_;
  size_t size_;
  size_t compressed_size_;
  std::string input_str_;
  std::string output_str_;
};

/* A kernel for compression */
class Compress : public CompressBase {
 public:
  explicit Compress(const CompressBase& base) : CompressBase(base) {}
  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override;
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override;
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    return std::make_shared<Compress>(
        CompressBase::config(config_d, "Hcompress", "Rcompressed"));
  }
};

/* A kernel for decompression */
class Decompress : public CompressBase {
 public:
  explicit Decompress(const CompressBase& base) : CompressBase(base) {}
  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override;
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs,
      std::shared_ptr<folly::Executor> pool) override;
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    return std::make_shared<Decompress>(
        CompressBase::config(config_d, "Hdecompress", "Rdecompressed"));
  }
};

} // namespace facebook::cea::chips::adsim

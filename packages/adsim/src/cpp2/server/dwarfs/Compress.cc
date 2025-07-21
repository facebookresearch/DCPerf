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

#include <lzbench.h>
#include <util.h>

#include <glog/logging.h>
#include <cstring>

#include <cea/chips/adsim/cpp2/server/dwarfs/Compress.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>

#include <fb303/ThreadCachedServiceData.h>
#include <folly/coro/Task.h>

extern int64_t lzbench_compress_helper(
    lzbench_params_t* params,
    std::vector<size_t>& chunk_sizes,
    compress_func compress,
    std::vector<size_t>& compr_sizes,
    uint8_t* inbuf,
    uint8_t* outbuf,
    size_t outsize,
    size_t param1,
    size_t param2,
    char* workmem);

extern int64_t lzbench_decompress_helper(
    lzbench_params_t* params,
    std::vector<size_t>& chunk_sizes,
    compress_func decompress,
    std::vector<size_t>& compr_sizes,
    uint8_t* inbuf,
    uint8_t* outbuf,
    size_t param1,
    size_t param2,
    char* workmem);

namespace facebook::cea::chips::adsim {

DECLARE_timeseries(compress_nfired);
DECLARE_timeseries(decompress_nfired);

/* Constructor of the base class
 *
 * @param  $encoder  Compression library (choose from zstd, lz4, zlib, bzip2)
 * @param  $level  Compression level
 * @param  $size  Size of the data to compress per kernel invocation
 * @param  $input_str  Kernel input data naming
 * @param  $output_str  Kernel output data naming
 * @return  A CompressBase instance.
 */
CompressBase::CompressBase(
    std::string encoder,
    int level,
    size_t size,
    std::string input_str,
    std::string output_str)
    : level_(level),
      size_(size),
      compressed_size_(GET_COMPRESS_BOUND(size_)),
      input_str_(input_str),
      output_str_(output_str) {
  for (int i = 1; i < LZBENCH_COMPRESSOR_COUNT; i++) {
    if (0 == strcmp(comp_desc[i].name, encoder.c_str())) {
      compressor_ = &comp_desc[i];
      break;
    }
  }
  if (nullptr == compressor_) {
    compressor_ = &comp_desc[0];
  }
}

/* Initialize the dataset for compression
 *
 * @param  $h_objs  Handler objects
 * @param  $r_objs  Request objects
 */
std::string CompressBase::init(
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<AdSimRequestObjs> r_objs) {
  Kernel::init(h_objs, r_objs);

  if (input_str_.empty() || 'H' != input_str_[0]) {
    return input_str_; // use data passed from other kernels via r_objs
  }

  void* buf = malloc(size_ + PAD_SIZE);
  int64_t* buf_i64 = (int64_t*)buf;
  for (int i = 0; (size_ >> 3) > i; ++i) {
    buf_i64[i] = get_rand();
  }
  h_objs->set_plain_ptr(input_str_ + ".buf_", buf);
  return folly::to<std::string>(
      input_str_, ".buf_ ", std::to_string(size_ + PAD_SIZE), "B");
}

/* Initialize the dataset for compression
 *
 * @param  $h_objs  Handler objects
 * @param  $r_objs  Request objects
 */
std::string Compress::init(
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<AdSimRequestObjs> r_objs) {
  return "Compress: " + CompressBase::init(h_objs, r_objs);
}

/* Invoke Compress kernel
 *
 * @param  $h_objs  Handler objects
 * @param  $r_objs  Request objects
 * @return  A Compress coroutine task
 */
folly::coro::Task<std::string> Compress::fire(
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<AdSimRequestObjs> r_objs,
    std::shared_ptr<folly::Executor> pool) {
  STATS_compress_nfired.add(1);
  // inc_nfired("Compress");
  // DLOG_EVERY_N(INFO, 1000) << "Compress fired " << get_nfired()["Compress"];
  lzbench_params_t params; // lzbench_compress doesn't really use it
  params.verbose = 0;
  std::vector<size_t> chunk_sizes, compr_sizes;
  uint8_t* outbuf = (uint8_t*)malloc(compressed_size_);
  char* workmem = (char*)outbuf; // keep linter happy. if no init, no access
  if (compressor_->init) { // if the compressor uses workmem, overwrite
    workmem = compressor_->init(size_, level_, 0);
  }
  size_t* compressed_size = (size_t*)malloc(sizeof(size_t));
  if (!input_str_.empty() && 'H' == input_str_[0]) {
    uint8_t* buf = (uint8_t*)h_objs->get_plain_ptr(input_str_ + ".buf_");
    chunk_sizes.emplace_back(size_);
    *compressed_size = lzbench_compress_helper(
        &params,
        chunk_sizes,
        compressor_->compress,
        compr_sizes,
        buf,
        outbuf,
        compressed_size_,
        level_,
        0,
        workmem);
  } else {
    auto bufq = r_objs->get_shared_ptr<folly::IOBufQueue>(input_str_);
    while (!bufq->empty()) {
      auto iobuf = bufq->pop_front();
      chunk_sizes.emplace_back(iobuf.get()->capacity());
      *compressed_size = lzbench_compress_helper(
          &params,
          chunk_sizes,
          compressor_->compress,
          compr_sizes,
          iobuf.get()->writableBuffer(),
          outbuf,
          compressed_size_,
          level_,
          0,
          workmem);
      chunk_sizes.pop_back();
    }
  }
  if (compressor_->deinit) {
    compressor_->deinit(workmem);
  }
  r_objs->set_plain_ptr(output_str_, outbuf);
  r_objs->set_plain_ptr(output_str_ + ".size_", compressed_size);
  co_return "";
}

/* Initialize the dataset for decompression
 *
 * @param  $h_objs  Handler objects
 * @param  $r_objs  Request objects
 */
std::string Decompress::init(
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<AdSimRequestObjs> r_objs) {
  auto buf_usage = CompressBase::init(h_objs, r_objs);

  if (input_str_.empty() || 'H' != input_str_[0]) {
    return "Decompress: " + input_str_;
  }

  uint8_t* buf = (uint8_t*)h_objs->get_plain_ptr(input_str_ + ".buf_");
  void* compressed_buf = malloc(compressed_size_);
  uint8_t* cbuf = (uint8_t*)compressed_buf;
  char* workmem = (char*)cbuf; // keep linter happy. if no init, no access
  if (compressor_->init) { // if the compressor uses workmem, overwrite
    workmem = compressor_->init(size_, level_, 0);
  }
  lzbench_params_t params; // lzbench_compress doesn't really use it
  params.verbose = 0;
  std::vector<size_t> chunk_sizes, compr_sizes;
  chunk_sizes.emplace_back(size_);
  // compress once first to fill the compressed_buf
  compressed_size_ = lzbench_compress_helper(
      &params,
      chunk_sizes,
      compressor_->compress,
      compr_sizes,
      buf,
      cbuf,
      size_,
      level_,
      0,
      workmem);
  if (compressor_->deinit) {
    compressor_->deinit(workmem);
  }
  h_objs->set_plain_ptr(input_str_ + ".compressed_buf_", compressed_buf);
  return folly::to<std::string>(
      "Decompress: ",
      buf_usage,
      " + ",
      input_str_,
      ".compressed_buf_ ",
      std::to_string(compressed_size_),
      "B");
}

/* Invoke Decompress kernel
 *
 * @param  $req  Request string (content doesn't matter)
 * @return  A Decompress coroutine task
 */
folly::coro::Task<std::string> Decompress::fire(
    std::shared_ptr<AdSimHandleObjs> h_objs,
    std::shared_ptr<AdSimRequestObjs> r_objs,
    std::shared_ptr<folly::Executor> pool) {
  STATS_decompress_nfired.add(1);
  // inc_nfired("Decompress");
  // DLOG_EVERY_N(INFO, 1000) << "Decompress fired " <<
  // get_nfired()["Decompress"];
  lzbench_params_t params; // lzbench_decompress doesn't really use it
  params.verbose = 0;
  std::vector<size_t> chunk_sizes, compr_sizes;
  uint8_t* outbuf = (uint8_t*)malloc(size_);
  char* workmem = (char*)outbuf; // keep linter happy. if no init, no access
  if (compressor_->init) { // if the compressor uses workmem, overwrite
    workmem = compressor_->init(size_, level_, 0);
  }
  if (!input_str_.empty() && 'H' == input_str_[0]) {
    uint8_t* compressed_buf =
        (uint8_t*)h_objs->get_plain_ptr(input_str_ + ".compressed_buf_");
    chunk_sizes.push_back(compressed_size_);
    lzbench_decompress_helper(
        &params,
        chunk_sizes,
        compressor_->decompress,
        compr_sizes,
        compressed_buf,
        outbuf,
        level_,
        0,
        workmem);
  } else {
    uint8_t* compressed_buf = (uint8_t*)r_objs->get_plain_ptr(input_str_);
    size_t* compressed_size =
        (size_t*)r_objs->get_plain_ptr(input_str_ + ".size_");
    compressed_size_ = *compressed_size;
    chunk_sizes.push_back(compressed_size_);
    lzbench_decompress_helper(
        &params,
        chunk_sizes,
        compressor_->decompress,
        compr_sizes,
        compressed_buf,
        outbuf,
        level_,
        0,
        workmem);
  }
  if (compressor_->deinit) {
    compressor_->deinit(workmem);
  }
  r_objs->set_plain_ptr(output_str_, outbuf);
  co_return "";
}

} // namespace facebook::cea::chips::adsim

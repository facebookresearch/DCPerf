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

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/DenseFP32Impl.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>

#include <folly/Conv.h>
#include <folly/coro/Task.h>
#include <folly/dynamic.h>

namespace facebook::cea::chips::adsim {

/* A kernel for an fp32 MLP: a chain of dense layers, each an fp32 GEMM of
 * `batch` activation rows against a prepacked weight matrix. It stands in for
 * the small fp32 dense layers an inference server runs on the host CPU.
 */
class DenseFP32 : public Kernel {
  using WeightSets = std::vector<dense_fp32::Layers>;

 public:
  /* Constructor of DenseFP32 kernel
   *
   * @param  $batch  Rows of the activation matrix (M of every GEMM)
   * @param  $dims  Layer widths; layer i multiplies by a dims[i] x dims[i+1]
   *                weight matrix
   * @param  $nweight_sets  Copies of the weights; each pass uses a random
   *                        copy, which sets the weight footprint
   * @param  $niters  Passes through the layer chain per fire
   * @param  $input_str  A string labeling the weights shared across fanouts;
   *                     kernels with the same label need the same dims and
   *                     nweight_sets
   */
  explicit DenseFP32(
      int batch,
      std::vector<int> dims,
      int nweight_sets,
      int niters,
      std::string input_str)
      : batch_(batch),
        dims_(std::move(dims)),
        nweight_sets_(nweight_sets),
        niters_(niters),
        input_str_(std::move(input_str)) {
    dense_fp32::validate(batch_, dims_, nweight_sets_, niters_);
  }

  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override {
    Kernel::init(h_objs, r_objs);
    weights_ = h_objs->get_shared_ptr_default<WeightSets>(
        input_str_ + ".weights", [&]() {
          return std::make_shared<WeightSets>(
              dense_fp32::makeWeightSets(dims_, nweight_sets_));
        });
    dense_fp32::validateShape(*weights_, dims_, nweight_sets_);
    size_t weight_bytes = 0;
    for (size_t l = 0; dims_.size() - 1 > l; ++l) {
      weight_bytes +=
          static_cast<size_t>(dims_[l]) * dims_[l + 1] * sizeof(float);
    }
    return folly::to<std::string>(
        "DenseFP32: ",
        input_str_,
        " ",
        weight_bytes * nweight_sets_,
        "B weights");
  }

  /* Invoke DenseFP32 kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects
   * @return  A DenseFP32 coroutine task
   */
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> /*unused*/,
      std::shared_ptr<AdSimRequestObjs> /*unused*/,
      std::shared_ptr<folly::Executor> /*unused*/) override {
    /* library-local */ thread_local dense_fp32::Workspace ws;
    for (int it = 0; niters_ > it; ++it) {
      const auto& layers = (*weights_)[get_rand() % nweight_sets_];
      dense_fp32::forward(layers, batch_, ws);
    }
    co_return "";
  }

  /* Builder of the DenseFP32 kernel, create an instance according to the
   * config
   *
   * @param  $config_d  Dynamic container of configuration
   * @return  A configured DenseFP32 kernel
   */
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    auto getInt = [&](const char* key, int dflt) {
      return folly::to<int>(config_d.getDefault(key, dflt).asInt());
    };
    std::vector<int> dims;
    for (const auto& d : config_d["dims"]) {
      dims.push_back(folly::to<int>(d.asInt()));
    }
    return std::make_shared<DenseFP32>(
        getInt("batch", 64),
        std::move(dims),
        getInt("nweight_sets", 1),
        getInt("niters", 1),
        config_d.getDefault("input", "densefp32").asString());
  }

 private:
  int batch_;
  std::vector<int> dims_;
  int nweight_sets_;
  int niters_;
  std::string input_str_;
  std::shared_ptr<WeightSets> weights_;
};

} // namespace facebook::cea::chips::adsim

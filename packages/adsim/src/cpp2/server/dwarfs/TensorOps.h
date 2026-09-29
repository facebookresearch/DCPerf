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
#include <memory>
#include <string>
#include <vector>

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/TensorOpsImpl.h>

#include <folly/Conv.h>
#include <folly/coro/Task.h>
#include <folly/dynamic.h>

namespace facebook::cea::chips::adsim {

/* A kernel that runs a configured sequence of CPU tensor operators (copy,
 * fill, add, LayerNorm, searchsorted, and sparse index helpers), modeling the
 * host-side tensor work of an inference server whose dense layers run on a
 * GPU.
 */
class TensorOps : public Kernel {
  using OpInputsList = std::vector<std::shared_ptr<tensor_ops::OpInputs>>;

 public:
  /* Constructor of TensorOps kernel
   *
   * @param  $specs  Operators to run per fire, in order
   * @param  $nbuffers  Copies of every operator's inputs; each invocation
   *                    reads a random copy
   * @param  $input_str  A string labeling the inputs shared across fanouts
   */
  explicit TensorOps(
      std::vector<tensor_ops::OpSpec> specs,
      int nbuffers,
      std::string input_str)
      : specs_(std::move(specs)),
        nbuffers_(nbuffers),
        input_str_(std::move(input_str)) {}

  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override {
    Kernel::init(h_objs, r_objs);
    inputs_ = h_objs->get_shared_ptr_default<OpInputsList>(
        input_str_ + ".inputs", [&]() {
          auto list = std::make_shared<OpInputsList>();
          for (size_t i = 0; specs_.size() > i; ++i) {
            list->push_back(
                std::make_shared<tensor_ops::OpInputs>(
                    specs_[i], nbuffers_, /*seed=*/i + 1));
          }
          return list;
        });
    int64_t in_bytes = 0;
    for (const auto& in : *inputs_) {
      in_bytes += in->inputElems() * sizeof(float) * in->nbuffers();
      out_bytes_ = std::max(out_bytes_, in->outputBytes());
    }
    return folly::to<std::string>(
        "TensorOps: ", input_str_, " ", in_bytes, "B inputs");
  }

  /* Invoke TensorOps kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects
   * @return  A TensorOps coroutine task
   */
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> /*unused*/,
      std::shared_ptr<AdSimRequestObjs> /*unused*/,
      std::shared_ptr<folly::Executor> /*unused*/) override {
    // Output workspace is per thread, like ATen's reuse of freed blocks. Each
    // copy is resized before use, so duplicate copies across libraries are
    // harmless.
    /* library-local */ thread_local std::vector<char> out;
    if (static_cast<int64_t>(out.size()) < out_bytes_) {
      out.resize(out_bytes_);
    }
    float sink = 0.f;
    for (const auto& in : *inputs_) {
      for (int rep = 0; in->spec().reps > rep; ++rep) {
        sink += tensor_ops::runOp(*in, get_rand() % in->nbuffers(), out.data());
      }
    }
    sink_.store(sink, std::memory_order_relaxed);
    co_return "";
  }

  /* Builder of the TensorOps kernel, create an instance according to the
   * config
   *
   * Each entry of "ops" is {"op", "rows", "cols", "aux", "reps"}; "aux" is
   * the searchsorted boundary count, cat part count, or index_select table
   * rows.
   *
   * @param  $config_d  Dynamic container of configuration
   * @return  A configured TensorOps kernel
   */
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    std::vector<tensor_ops::OpSpec> specs;
    for (const auto& op_d : config_d["ops"]) {
      specs.push_back(
          tensor_ops::OpSpec{
              tensor_ops::parseOpKind(op_d["op"].asString()),
              op_d.getDefault("rows", 64).asInt(),
              op_d.getDefault("cols", 256).asInt(),
              op_d.getDefault("aux", 4).asInt(),
              static_cast<int>(op_d.getDefault("reps", 1).asInt())});
    }
    int nbuffers =
        static_cast<int>(config_d.getDefault("nbuffers", 16).asInt());
    std::string input_str =
        config_d.getDefault("input", "tensorops").asString();
    return std::make_shared<TensorOps>(std::move(specs), nbuffers, input_str);
  }

 private:
  std::vector<tensor_ops::OpSpec> specs_;
  int nbuffers_;
  std::string input_str_;
  std::shared_ptr<OpInputsList> inputs_;
  int64_t out_bytes_ = 0;
  // Keeps the operator results live; concurrent fires may overwrite it.
  std::atomic<float> sink_{0.f};
};

} // namespace facebook::cea::chips::adsim

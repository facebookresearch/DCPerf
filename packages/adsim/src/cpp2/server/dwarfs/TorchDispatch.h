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

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/TorchDispatchImpl.h>

#include <folly/Conv.h>
#include <folly/coro/Task.h>
#include <folly/dynamic.h>

namespace facebook::cea::chips::adsim {

/* A kernel that models the host runtime of a TorchScript model: an
 * interpreter over boxed values, per-op dispatch through a key-indexed table,
 * reference counting, and small tensor allocations, across a broad set of
 * distinct op kernels. It stands in for the c10 dispatcher and TorchScript
 * interpreter overhead of an inference server whose dense layers run on a
 * GPU.
 */
class TorchDispatch : public Kernel {
  using Programs = std::vector<torch_dispatch::Code>;

 public:
  /* Constructor of TorchDispatch kernel
   *
   * @param  $spec  Shape of the generated graph
   * @param  $nprograms  Distinct graphs; each run picks a random one
   * @param  $nruns  Graph runs per fire
   * @param  $rows  Rows of each input tensor
   * @param  $cols  Columns of each input tensor
   * @param  $input_str  A string labeling the graphs shared across fanouts
   */
  explicit TorchDispatch(
      torch_dispatch::GraphSpec spec,
      int nprograms,
      int nruns,
      int64_t rows,
      int64_t cols,
      std::string input_str)
      : spec_(spec),
        nprograms_(std::max(1, nprograms)),
        nruns_(nruns),
        rows_(rows),
        cols_(cols),
        input_str_(std::move(input_str)) {}

  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override {
    Kernel::init(h_objs, r_objs);
    programs_ = h_objs->get_shared_ptr_default<Programs>(
        input_str_ + ".programs", [&]() {
          auto programs = std::make_shared<Programs>();
          for (int i = 0; nprograms_ > i; ++i) {
            torch_dispatch::GraphSpec g = spec_;
            g.seed = spec_.seed + i;
            programs->push_back(torch_dispatch::buildCode(g));
          }
          return programs;
        });
    return folly::to<std::string>(
        "TorchDispatch: ",
        input_str_,
        " ",
        (*programs_)[0].instrs.size(),
        " instrs x ",
        nprograms_,
        " programs");
  }

  /* Invoke TorchDispatch kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects
   * @return  A TorchDispatch coroutine task
   */
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> /*unused*/,
      std::shared_ptr<AdSimRequestObjs> /*unused*/,
      std::shared_ptr<folly::Executor> /*unused*/) override {
    std::vector<torch_dispatch::IValue> regs;
    torch_dispatch::Stack stack;
    stack.reserve(16);
    int64_t live = 0;
    for (int i = 0; nruns_ > i; ++i) {
      const auto& code = (*programs_)[get_rand() % nprograms_];
      live += torch_dispatch::runOnce(code, rows_, cols_, regs, stack);
    }
    sink_.store(live, std::memory_order_relaxed);
    co_return "";
  }

  /* Builder of the TorchDispatch kernel, create an instance according to the
   * config
   *
   * "nops", "ninstr_ops", "nregs", "ninputs", "zipf_s", "work_cap",
   * "meta_frac", "shared_meta_frac", "chase_len", and "chase_nodes" set the
   * GraphSpec fields of the same names.
   *
   * @param  $config_d  Dynamic container of configuration
   * @return  A configured TorchDispatch kernel
   */
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    torch_dispatch::GraphSpec spec;
    auto getInt = [&](const char* key, int dflt) {
      return folly::to<int>(config_d.getDefault(key, dflt).asInt());
    };
    spec.nops = getInt("nops", spec.nops);
    spec.ninstr_ops = getInt("ninstr_ops", spec.ninstr_ops);
    spec.nregs = getInt("nregs", spec.nregs);
    spec.ninputs = getInt("ninputs", spec.ninputs);
    spec.zipf_s = config_d.getDefault("zipf_s", spec.zipf_s).asDouble();
    spec.work_cap = getInt("work_cap", spec.work_cap);
    spec.meta_frac =
        config_d.getDefault("meta_frac", spec.meta_frac).asDouble();
    spec.shared_meta_frac =
        config_d.getDefault("shared_meta_frac", spec.shared_meta_frac)
            .asDouble();
    spec.chase_len = getInt("chase_len", spec.chase_len);
    spec.chase_nodes = getInt("chase_nodes", spec.chase_nodes);
    return std::make_shared<TorchDispatch>(
        spec,
        getInt("nprograms", 1),
        getInt("nruns", 1),
        config_d.getDefault("rows", 32).asInt(),
        config_d.getDefault("cols", 64).asInt(),
        config_d.getDefault("input", "torchdispatch").asString());
  }

 private:
  torch_dispatch::GraphSpec spec_;
  int nprograms_;
  int nruns_;
  int64_t rows_;
  int64_t cols_;
  std::string input_str_;
  std::shared_ptr<Programs> programs_;
  // Keeps the run results live; concurrent fires may overwrite it.
  std::atomic<int64_t> sink_{0};
};

} // namespace facebook::cea::chips::adsim

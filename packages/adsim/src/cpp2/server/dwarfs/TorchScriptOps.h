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

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/TorchScriptOpsImpl.h>

#include <folly/Conv.h>
#include <folly/ThreadLocal.h>
#include <folly/coro/Task.h>
#include <folly/dynamic.h>

namespace facebook::cea::chips::adsim {

/* A kernel that runs a TorchScript program through libtorch's interpreter,
 * dispatcher, and ATen CPU kernels: a list of operators, a generated graph of
 * small ops, and a pointer chase through index_select.
 */
class TorchScriptOps : public Kernel {
 public:
  /* Constructor of TorchScriptOps kernel
   *
   * @param  $specs  Operators to run per fire, in order
   * @param  $nbuffers  Copies of every operator's inputs; each invocation
   *                    reads a random copy
   * @param  $graph  Generated graph to run after the operators
   * @param  $chase  Pointer chase to run after the graph
   * @param  $input_str  A string labeling the program shared across fanouts
   */
  explicit TorchScriptOps(
      std::vector<torchscript_ops::OpSpec> specs,
      int nbuffers,
      torchscript_ops::GraphSpec graph,
      torchscript_ops::ChaseSpec chase,
      std::string input_str)
      : specs_(std::move(specs)),
        nbuffers_(nbuffers),
        graph_(std::move(graph)),
        chase_(chase),
        input_str_(std::move(input_str)) {}

  std::string init(
      std::shared_ptr<AdSimHandleObjs> h_objs,
      std::shared_ptr<AdSimRequestObjs> r_objs = nullptr) override {
    Kernel::init(h_objs, r_objs);
    program_ = h_objs->get_shared_ptr_default<torchscript_ops::Program>(
        input_str_ + ".program", [&]() {
          return torchscript_ops::compileProgram(
              specs_, nbuffers_, graph_, chase_);
        });
    return folly::to<std::string>(
        "TorchScriptOps: ", input_str_, " ", program_->describe());
  }

  /* Invoke TorchScriptOps kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects
   * @return  A TorchScriptOps coroutine task
   */
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> /*unused*/,
      std::shared_ptr<AdSimRequestObjs> /*unused*/,
      std::shared_ptr<folly::Executor> /*unused*/) override {
    // Create this thread's frame
    if (!frames_) {
      frames_.reset(program_->newFrame(get_rand()));
    }
    frames_->fire();
    co_return "";
  }

  /* Builder of the TorchScriptOps kernel, create an instance according to
   * the config
   *
   * "ops" lists the operators ("op", "rows", "cols", "aux", "reps"), with
   * "nbuffers" copies of their inputs. "nops", "nruns", "nregs", "ninputs",
   * "rows", "cols", "zipf_s", "cover", "templates", "seed", and "dtypes"
   * shape the generated graph, which runs if "nops" is positive.
   * "chase_len", "chase_lines", and "chase_width" shape the pointer chase,
   * which runs if "chase_len" is positive.
   *
   * @param  $config_d  Dynamic container of configuration
   * @return  A configured TorchScriptOps kernel
   */
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    std::vector<torchscript_ops::OpSpec> specs;
    if (const auto* ops_d = config_d.get_ptr("ops")) {
      for (const auto& op_d : *ops_d) {
        specs.push_back(
            torchscript_ops::OpSpec{
                torchscript_ops::parseOpKind(op_d["op"].asString()),
                op_d.getDefault("rows", 64).asInt(),
                op_d.getDefault("cols", 256).asInt(),
                op_d.getDefault("aux", 4).asInt(),
                static_cast<int>(op_d.getDefault("reps", 1).asInt())});
      }
    }
    torchscript_ops::GraphSpec graph;
    graph.nops =
        static_cast<int>(config_d.getDefault("nops", graph.nops).asInt());
    graph.nruns =
        static_cast<int>(config_d.getDefault("nruns", graph.nruns).asInt());
    graph.nregs =
        static_cast<int>(config_d.getDefault("nregs", graph.nregs).asInt());
    graph.ninputs =
        static_cast<int>(config_d.getDefault("ninputs", graph.ninputs).asInt());
    graph.rows = config_d.getDefault("rows", graph.rows).asInt();
    graph.cols = config_d.getDefault("cols", graph.cols).asInt();
    graph.zipf_s = config_d.getDefault("zipf_s", graph.zipf_s).asDouble();
    graph.cover = config_d.getDefault("cover", graph.cover).asBool();
    if (const auto* templates_d = config_d.get_ptr("templates")) {
      for (const auto& name_d : *templates_d) {
        graph.templates.push_back(name_d.asString());
      }
    }
    graph.seed = static_cast<uint64_t>(config_d.getDefault("seed", 1).asInt());
    if (const auto* dtypes_d = config_d.get_ptr("dtypes")) {
      graph.dtypes.clear();
      for (const auto& dtype_d : *dtypes_d) {
        graph.dtypes.push_back(dtype_d.asString());
      }
    }
    torchscript_ops::ChaseSpec chase;
    chase.len =
        static_cast<int>(config_d.getDefault("chase_len", chase.len).asInt());
    chase.lines = config_d.getDefault("chase_lines", chase.lines).asInt();
    chase.width = static_cast<int>(
        config_d.getDefault("chase_width", chase.width).asInt());
    chase.seed = graph.seed;
    return std::make_shared<TorchScriptOps>(
        std::move(specs),
        static_cast<int>(config_d.getDefault("nbuffers", 16).asInt()),
        std::move(graph),
        chase,
        config_d.getDefault("input", "torchscriptops").asString());
  }

 private:
  std::vector<torchscript_ops::OpSpec> specs_;
  int nbuffers_;
  torchscript_ops::GraphSpec graph_;
  torchscript_ops::ChaseSpec chase_;
  std::string input_str_;
  std::shared_ptr<torchscript_ops::Program> program_;
  folly::ThreadLocalPtr<torchscript_ops::Frame> frames_;
};

} // namespace facebook::cea::chips::adsim

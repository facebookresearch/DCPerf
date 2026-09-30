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

#include <cea/chips/adsim/cpp2/server/DataObjects.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/GpuSpinWaitImpl.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Kernel.h>

#include <folly/Conv.h>
#include <folly/coro/Task.h>
#include <folly/dynamic.h>
#include <glog/logging.h>

namespace facebook::cea::chips::adsim {

/* A kernel that models the host side of waiting for a GPU kernel to finish:
 * a GPU runtime's busy-wait on a completion signal. Each wait spins on the
 * signal and the clock until the emulated GPU time has passed, parking in
 * MONITORX/MWAITX between checks when enabled, as ROCm's runtime does on CPUs
 * that support it.
 */
class GpuSpinWait : public Kernel {
 public:
  /* Constructor of GpuSpinWait kernel
   *
   * @param  $wait_us  Emulated GPU execution time of each dispatch, in us
   * @param  $nwaits  Number of dispatches waited on per fire
   * @param  $use_mwaitx  Park in MWAITX between checks; ignored with a
   *                      warning if the CPU lacks MONITORX/MWAITX
   */
  explicit GpuSpinWait(double wait_us, int nwaits, bool use_mwaitx)
      : wait_us_(wait_us),
        nwaits_(nwaits),
        use_mwaitx_(use_mwaitx && gpu_spin_wait::cpuHasMwaitx()) {
    gpu_spin_wait::validate(wait_us_, nwaits_);
    if (use_mwaitx && !use_mwaitx_) {
      LOG(WARNING) << "CPU lacks MONITORX/MWAITX; GpuSpinWait spins without it";
    }
  }

  std::string init(
      std::shared_ptr<AdSimHandleObjs> /*unused*/,
      std::shared_ptr<AdSimRequestObjs> /*unused*/) override {
    busy_ticks_ = gpu_spin_wait::busyTicks(wait_us_);
    return "GpuSpinWait";
  }

  /* Invoke GpuSpinWait kernel
   *
   * @param  $h_objs  Handler objects
   * @param  $r_objs  Request objects
   * @return  A GpuSpinWait coroutine task
   */
  folly::coro::Task<std::string> fire(
      std::shared_ptr<AdSimHandleObjs> /*unused*/,
      std::shared_ptr<AdSimRequestObjs> /*unused*/,
      std::shared_ptr<folly::Executor> /*unused*/) override {
    /* library-local */ thread_local gpu_spin_wait::Signal signal;
    for (int i = 0; nwaits_ > i; ++i) {
      gpu_spin_wait::waitForDispatch(signal, busy_ticks_, use_mwaitx_);
    }
    co_return "";
  }

  /* Builder of the GpuSpinWait kernel, create an instance according to the
   * config
   *
   * @param  $config_d  Dynamic container of configuration
   * @return  A configured GpuSpinWait kernel
   */
  static std::shared_ptr<Kernel> config(const folly::dynamic& config_d) {
    return std::make_shared<GpuSpinWait>(
        config_d.getDefault("wait_us", 20.0).asDouble(),
        folly::to<int>(config_d.getDefault("nwaits", 1).asInt()),
        config_d.getDefault("use_mwaitx", true).asBool());
  }

 private:
  double wait_us_;
  int nwaits_;
  bool use_mwaitx_;
  uint64_t busy_ticks_ = 0;
};

} // namespace facebook::cea::chips::adsim

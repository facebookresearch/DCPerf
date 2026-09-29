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

// A host thread busy-waiting for a GPU dispatch to finish, following the
// completion-signal wait in ROCm's ROCr runtime. There is no GPU: the waiter
// completes the signal itself once the emulated execution time has passed.
// The code depends only on the standard library and x86 intrinsics so it can
// be tested without AdSim.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <stdexcept>
#include <string>

#if defined(__x86_64__)
#include <cpuid.h>
#include <x86intrin.h>
#endif

namespace facebook::cea::chips::adsim::gpu_spin_wait {

// ROCr's MWAITX timeout per wait step, in TSC ticks.
constexpr uint32_t kMwaitxTimeoutTicks = 60000;
// ECX bit 1 of MWAITX enables the EBX timeout.
constexpr uint32_t kMwaitxTimerEnable = 0x2;

/* The clock ROCr reads while waiting: the TSC on x86, and nanoseconds of
 * CLOCK_MONOTONIC_RAW elsewhere.
 */
inline uint64_t nowTicks() {
#if defined(__x86_64__)
  return __rdtsc();
#else
  struct timespec ts{};
  clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000 +
      static_cast<uint64_t>(ts.tv_nsec);
#endif
}

/* Ticks of nowTicks() per microsecond, measured once against steady_clock
 * over 20 ms.
 */
inline double ticksPerUs() {
  static const double kTicksPerUs = [] {
    const auto t0 = std::chrono::steady_clock::now();
    const uint64_t c0 = nowTicks();
    while (std::chrono::steady_clock::now() - t0 <
           std::chrono::milliseconds(20)) {
    }
    const uint64_t c1 = nowTicks();
    const double us = std::chrono::duration<double, std::micro>(
                          std::chrono::steady_clock::now() - t0)
                          .count();
    return static_cast<double>(c1 - c0) / us;
  }();
  return kTicksPerUs;
}

/* Whether the CPU has MONITORX/MWAITX (CPUID Fn8000_0001 ECX bit 29). */
inline bool cpuHasMwaitx() {
#if defined(__x86_64__)
  unsigned int eax = 0;
  unsigned int ebx = 0;
  unsigned int ecx = 0;
  unsigned int edx = 0;
  return __get_cpuid(0x80000001, &eax, &ebx, &ecx, &edx) &&
      0 != (ecx & (1u << 29));
#else
  return false;
#endif
}

#if defined(__x86_64__)
/* Parks until `addr` is written or `timeout` TSC ticks pass, unless it no
 * longer holds `last`.
 */
__attribute__((target("mwaitx"))) inline void
parkMwaitx(std::atomic<int64_t>* addr, int64_t last, uint32_t timeout) {
  _mm_monitorx(addr, 0, 0);
  if (addr->load(std::memory_order_relaxed) != last) {
    return;
  }
  _mm_mwaitx(kMwaitxTimerEnable, 0, timeout);
}
#endif

/* Completion signal of one emulated GPU dispatch, alone on its cache line. */
struct alignas(64) Signal {
  std::atomic<int64_t> value{0};
};

/* Waits for one emulated GPU dispatch that completes after `busy_ticks`.
 *
 * The loop follows rocr::core::BusyWaitSignal::WaitRelaxed: load the signal,
 * test it, read the clock, and park in MONITORX/MWAITX between checks. The
 * waiter completes the signal itself once `busy_ticks` have elapsed, and the
 * MWAITX timeout is capped at the time left so the wait ends on schedule.
 *
 * @param  $signal  The signal to wait on
 * @param  $busy_ticks  Emulated GPU execution time in nowTicks() units
 * @param  $use_mwaitx  Park in MWAITX between checks; the CPU must support it
 * @return  Number of loop iterations executed
 */
inline int64_t
waitForDispatch(Signal& signal, uint64_t busy_ticks, bool use_mwaitx) {
  signal.value.store(1, std::memory_order_relaxed);
  const uint64_t start = nowTicks();
  int64_t iters = 0;
  while (true) {
    ++iters;
    const int64_t value = signal.value.load(std::memory_order_relaxed);
    if (0 == value) {
      return iters;
    }
    const uint64_t elapsed = nowTicks() - start;
    if (elapsed >= busy_ticks) {
      signal.value.store(0, std::memory_order_relaxed);
      continue;
    }
#if defined(__x86_64__)
    if (use_mwaitx) {
      parkMwaitx(
          &signal.value,
          value,
          static_cast<uint32_t>(
              std::min<uint64_t>(kMwaitxTimeoutTicks, busy_ticks - elapsed)));
    }
#endif
  }
}

/* `wait_us` microseconds in nowTicks() units. */
inline uint64_t busyTicks(double wait_us) {
  return static_cast<uint64_t>(wait_us * ticksPerUs());
}

/* Throws std::invalid_argument unless `wait_us` is finite and non-negative
 * and `nwaits` is positive.
 */
inline void validate(double wait_us, int nwaits) {
  if (!std::isfinite(wait_us) || 0 > wait_us) {
    throw std::invalid_argument(
        "GpuSpinWait wait_us must be finite and non-negative, got " +
        std::to_string(wait_us));
  }
  if (1 > nwaits) {
    throw std::invalid_argument(
        "GpuSpinWait nwaits must be positive, got " + std::to_string(nwaits));
  }
}

} // namespace facebook::cea::chips::adsim::gpu_spin_wait

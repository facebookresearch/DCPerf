/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * dr_trace.h - DynamoRIO Trace Instrumentation
 *
 * Provides several tracing methods for use with drmemtrace_static.
 * All methods produce offline traces in an output directory.
 *
 * Methods:
 *   trace_start() / trace_stop()       - Direct start/stop at code locations
 *   trace_execution_pipe()             - Wait for pipe trigger, parse duration
 *   trace_execution_delay<S,D>()       - Wait S seconds, trace for D seconds
 *   trace_execution_roi<N,D>()         - Wait for N ROI hits, trace D seconds
 *
 * Configuration:
 *   If DYNAMORIO_OPTIONS is already set in the environment, it takes
 *   precedence over all configuration. Call trace_configure() before
 *   any tracing function to customize the output directory, verbosity,
 *   or assertion handling. If not called, defaults are used.
 *
 * Safety:
 *   trace_begin() and trace_end() are safe by default:
 *   - trace_begin() is idempotent: calling it while already tracing is a
 *     no-op (logs a warning).
 *   - trace_end() is idempotent: calling it without a matching
 *     trace_begin(), or calling it twice, is a no-op (logs a warning).
 *   - If max_trace_seconds > 0, trace_begin() spawns a watchdog thread
 *     that auto-stops tracing after the timeout. This prevents runaway
 *     traces from filling disk (traces are 100+ GB for multi-threaded
 *     workloads). The watchdog and trace_end() use an atomic CAS to
 *     ensure only one of them actually stops tracing.
 */

#pragma once

// DynamoRIO requires target OS and architecture macros before inclusion.
#define LINUX 1
#if defined(__x86_64__)
#define X86_64 1
#elif defined(__aarch64__)
#define ARM_64 1
#endif

#include <dr_api.h>
#include <fcntl.h>
#include <glog/logging.h>
#include <sys/stat.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <thread>

/* ---- Configuration ---- */

struct DrTraceConfig {
  const char* outdir = "/tmp/drmemtrace_out";
  int verbose = 3;
  bool ignore_asserts = true;
  uint32_t max_trace_seconds =
      0; // watchdog timeout in seconds (0 = no timeout)
};

/**
 * Overrides default tracing configuration.
 * Call before any tracing function.
 */
void trace_configure(const DrTraceConfig& config);

/**
 * Configure tracing from environment variables.
 * Reads DR_TRACE_OUTDIR, DR_TRACE_VERBOSE, DR_TRACE_IGNORE_ASSERTS,
 * and DR_TRACE_MAX_TRACE_SECONDS. Unset variables keep their defaults.
 */
void trace_configure_env(void);

/**
 * Create output directory and set DYNAMORIO_OPTIONS from DrTraceConfig.
 * Safe to call multiple times. Call before trace_begin().
 */
void trace_init(void);

/**
 * Start DynamoRIO instrumentation.
 * Idempotent: no-op if already tracing.
 * Spawns a watchdog thread if max_trace_seconds > 0.
 * The watchdog is spawned BEFORE DR starts (creating threads while DR is
 * active causes "Failed to take over all threads" errors).
 */
void trace_begin(void);

/**
 * Stop DynamoRIO instrumentation.
 * Idempotent: no-op if not currently tracing. Safe against concurrent
 * calls (e.g. watchdog + explicit stop).
 */
void trace_end(void);

/* ---- Direct start/stop ---- */

/**
 * Convenience wrapper: trace_init() + trace_begin().
 */
void trace_start(void);

/**
 * Convenience wrapper: trace_end().
 */
void trace_stop(void);

/* ---- Pipe-triggered (background thread) ---- */

/**
 * Wait for an external trigger via named pipe, then trace.
 *
 * Usage:
 *   std::thread bg(trace_execution_pipe);
 *   // ... application runs ...
 *   // In another terminal:
 *   //   echo 30 > <outdir>/dr_trace_trigger   # trace for 30 seconds
 *   //   echo go > <outdir>/dr_trace_trigger   # trace for 10 seconds (default)
 *   bg.join();
 *
 * The pipe input is parsed as a duration in seconds. Non-numeric input
 * (e.g. "go") defaults to 10 seconds. The actual duration is recorded
 * in <outdir>/trace_info.txt.
 */
void trace_execution_pipe(void);

/* ---- Delay-based (background thread) ---- */

/**
 * Wait StartDelaySeconds, then trace for TraceDurationSeconds.
 *
 * Template parameters are compile-time constants to ensure any two
 * traces from the same binary use the exact same methodology.
 *
 * Usage:
 *   std::thread bg(trace_execution_delay<30, 10>);
 *   bg.join();
 */
template <uint32_t StartDelaySeconds, uint32_t TraceDurationSeconds>
void trace_execution_delay(void) {
  trace_init();

  LOG(INFO) << "Sleeping for " << StartDelaySeconds
            << " seconds before starting tracing...";
  // NOLINTNEXTLINE(facebook-hte-BadCall-sleep_for)
  std::this_thread::sleep_for(std::chrono::seconds(StartDelaySeconds));

  trace_begin();
  // NOLINTNEXTLINE(facebook-hte-BadCall-sleep_for)
  std::this_thread::sleep_for(std::chrono::seconds(TraceDurationSeconds));
  trace_end();
}

/* ---- ROI-based (background thread) ---- */

// NOLINTNEXTLINE(facebook-avoid-non-const-global-variables)
extern std::atomic<uint32_t> dr_trace_roi_count;
// NOLINTNEXTLINE(facebook-avoid-non-const-global-variables)
extern std::atomic<bool> dr_trace_roi_log;

/**
 * Increment the ROI counter. Call at the start of kernels to trace.
 */
void trace_roi_hit(void);

/**
 * Wait for StartROICount ROI hits, then trace for TraceDurationSeconds.
 *
 * Template parameters are compile-time constants to ensure any two
 * traces from the same binary use the exact same methodology.
 *
 * Usage:
 *   std::thread bg(trace_execution_roi<100, 10>);
 *   while (running) { trace_roi_hit(); run_kernel(); }
 *   bg.join();
 */
template <uint32_t StartROICount, uint32_t TraceDurationSeconds>
void trace_execution_roi(void) {
  trace_init();

  LOG(INFO) << "Waiting for " << StartROICount
            << " ROI hits before starting tracing...";

  while (dr_trace_roi_count.load(std::memory_order_relaxed) < StartROICount) {
    // NOLINTNEXTLINE(facebook-hte-BadCall-sleep_for)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  dr_trace_roi_log.store(false, std::memory_order_relaxed);

  trace_begin();
  // NOLINTNEXTLINE(facebook-hte-BadCall-sleep_for)
  std::this_thread::sleep_for(std::chrono::seconds(TraceDurationSeconds));
  trace_end();
}

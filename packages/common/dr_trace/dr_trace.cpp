/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * dr_trace.cpp - DynamoRIO Trace Instrumentation
 *
 * Implementation of dr_trace.h, see header for more details.
 */

#include "dr_trace.h"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

/* ---- Global configuration ---- */

// NOLINTNEXTLINE(facebook-avoid-non-const-global-variables)
static DrTraceConfig g_config;
// NOLINTNEXTLINE(facebook-avoid-non-const-global-variables)
static std::atomic<bool> g_tracing_active{false};
// NOLINTNEXTLINE(facebook-avoid-non-const-global-variables)
std::atomic<bool> dr_trace_roi_log{true};
// NOLINTNEXTLINE(facebook-avoid-non-const-global-variables)
std::atomic<uint32_t> dr_trace_roi_count{0};

void trace_configure(const DrTraceConfig& config) {
  g_config = config;
}

void trace_configure_env(void) {
  DrTraceConfig cfg;

  const char* val = getenv("DR_TRACE_OUTDIR");
  if (val != nullptr) {
    cfg.outdir = val;
  }

  val = getenv("DR_TRACE_VERBOSE");
  if (val != nullptr) {
    cfg.verbose = atoi(val);
  }

  val = getenv("DR_TRACE_IGNORE_ASSERTS");
  if (val != nullptr) {
    cfg.ignore_asserts = (atoi(val) != 0);
  }

  val = getenv("DR_TRACE_MAX_TRACE_SECONDS");
  if (val != nullptr) {
    cfg.max_trace_seconds = static_cast<uint32_t>(atoi(val));
  }

  g_config = cfg;
}

/* ---- Forward Declarations ---- */

void trace_init(void);
void trace_begin(void);
void trace_end(void);
void trace_start_watchdog(void);

/* ---- Direct start/stop hooks ---- */

void trace_start(void) {
  trace_init();
  trace_begin();
}

void trace_stop(void) {
  trace_end();
}

/* ---- Pipe-triggered hooks ---- */

static void write_trace_info(const char* outdir, uint32_t duration_secs) {
  std::string path = std::string(outdir) + "/trace_info.txt";
  FILE* f = fopen(path.c_str(), "w");
  if (!f) {
    return;
  }

  time_t now = time(nullptr);
  struct tm tm_buf{};
  localtime_r(&now, &tm_buf);
  char timebuf[64];
  strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_buf);

  fprintf(f, "trace_start_time: %s\n", timebuf);
  fprintf(f, "trace_duration_secs: %u\n", duration_secs);
  fprintf(f, "outdir: %s\n", outdir);
  fclose(f);
}

void trace_execution_pipe(void) {
  trace_init();

  std::string pipe_path = std::string(g_config.outdir) + "/dr_trace_trigger";
  unlink(pipe_path.c_str());
  if (mkfifo(pipe_path.c_str(), 0644) != 0 && errno != EEXIST) {
    LOG(ERROR) << "Failed to create trigger pipe " << pipe_path
               << " (errno=" << errno << ")";
    return;
  }
  LOG(INFO) << "Waiting for trace trigger: echo [duration_secs] > "
            << pipe_path;

  uint32_t duration = 10;
  {
    int fd = open(pipe_path.c_str(), O_RDONLY);
    if (fd < 0) {
      LOG(ERROR) << "Failed to open trigger pipe " << pipe_path
                 << " (errno=" << errno << ")";
      unlink(pipe_path.c_str());
      return;
    }
    char buf[64] = {};
    read(fd, buf, sizeof(buf) - 1);
    close(fd);
    unlink(pipe_path.c_str());

    int parsed = atoi(buf);
    if (parsed > 0) {
      duration = static_cast<uint32_t>(parsed);
    }
  }
  LOG(INFO) << "Trace trigger received! Duration: " << duration << " seconds";

  write_trace_info(g_config.outdir, duration);

  trace_begin();
  // NOLINTNEXTLINE(facebook-hte-BadCall-sleep)
  sleep(duration);
  trace_end();
}

/* ---- Support for ROI hooks ---- */

void trace_roi_hit(void) {
  if (dr_trace_roi_log.load(std::memory_order_relaxed)) {
    dr_trace_roi_count.fetch_add(1, std::memory_order_relaxed);
  }
}

/* ---- Core functions ---- */

// Create output directory and set DYNAMORIO_OPTIONS env var from
// DrTraceConfig. If DYNAMORIO_OPTIONS is already set, it is left
// unchanged. Safe to call multiple times.
void trace_init(void) {
  mkdir(g_config.outdir, 0755);

  if (getenv("DYNAMORIO_OPTIONS") != nullptr) {
    return;
  }

  std::string opts =
      "-rstats_to_stderr -no_hook_vsyscall"
      " -disable_traces -no_enable_reset";

  if (g_config.ignore_asserts) {
    opts +=
        " -ignore_assert_list"
        " '/mnt/srcs/dynamorio/core/unix/os.c:10307"
        ";/mnt/srcs/dynamorio/core/unix/os.c:557'";
  }

  opts +=
      " -client_lib ';;-offline"
      " -verbose " +
      std::to_string(g_config.verbose) + " -outdir " + g_config.outdir + "'";

  int result = setenv("DYNAMORIO_OPTIONS", opts.c_str(), 1);
  CHECK_EQ(result, 0) << "Failed to set DYNAMORIO_OPTIONS";
}

// Start DynamoRIO instrumentation. Idempotent: no-op if already tracing.
// Spawns a watchdog thread (before DR) if max_trace_seconds > 0.
void trace_begin(void) {
  bool expected = false;
  if (!g_tracing_active.compare_exchange_strong(expected, true)) {
    LOG(WARNING) << "trace_begin() called while already tracing — ignoring";
    return;
  }

  // Spawn watchdog BEFORE dr_app_setup_and_start() — creating threads while
  // DR is active causes "Failed to take over all threads" errors.
  if (g_config.max_trace_seconds > 0) {
    trace_start_watchdog();
  }

  LOG(INFO) << "Starting tracing...";
  dr_app_setup_and_start();
  CHECK(dr_app_running_under_dynamorio())
      << "Failed to start DynamoRIO instrumentation";
  LOG(INFO) << "Running under DR " << std::boolalpha
            << dr_app_running_under_dynamorio();
}

// Stop DynamoRIO instrumentation. Idempotent: no-op if not tracing.
// Safe against concurrent calls (watchdog + explicit stop).
void trace_end(void) {
  bool expected = true;
  if (!g_tracing_active.compare_exchange_strong(expected, false)) {
    LOG(WARNING) << "trace_end() called while not tracing — ignoring";
    return;
  }

  LOG(INFO) << "Stopping tracing...";
  dr_app_stop_and_cleanup();
  LOG(INFO) << "Stopped tracing.";
}

/* ---- Watchdog ---- */

// Start watchdog thread to stop tracing when time limit is reached.
void trace_start_watchdog(void) {
  uint32_t timeout = g_config.max_trace_seconds;
  if (timeout == 0) {
    return;
  }

  std::thread watchdog([timeout] {
    // NOLINTNEXTLINE(facebook-hte-BadCall-sleep_for)
    std::this_thread::sleep_for(std::chrono::seconds(timeout));
    bool exp = true;
    if (g_tracing_active.compare_exchange_strong(exp, false)) {
      LOG(WARNING) << "Trace auto-stopped after " << timeout
                   << "s (watchdog timeout)";
      dr_app_stop_and_cleanup();
      LOG(INFO) << "Stopped tracing.";
    }
  });
  // NOLINTNEXTLINE(facebook-hte-BadCall-detach)
  watchdog.detach();
}

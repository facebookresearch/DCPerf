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
#include <vector>

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

/* ---- Pagemap dump ---- */

// Binary format written to pagemap_{pre,post}_trace.bin:
//   Repeated entries of:
//     uint64_t virt_page_addr   (page-aligned virtual address)
//     uint64_t pagemap_entry    (raw /proc/self/pagemap entry, 8 bytes)
//   Only pages with the "present" bit set (bit 63) are written.

struct MapsRange {
  uint64_t start;
  uint64_t end;
};

static std::vector<MapsRange> parse_self_maps() {
  std::vector<MapsRange> ranges;
  FILE* f = fopen("/proc/self/maps", "r");
  if (!f) {
    return ranges;
  }
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    uint64_t start = 0;
    uint64_t end = 0;
    if (sscanf(line, "%lx-%lx", &start, &end) == 2) {
      ranges.push_back({start, end});
    }
  }
  fclose(f);
  return ranges;
}

static constexpr uint64_t kPageSize = 4096;
static constexpr uint64_t kPagemapEntrySize = 8;
static constexpr uint64_t kPresentBit = 1ULL << 63;
static constexpr uint64_t kPfnMask = 0x007FFFFFFFFFFFFFULL;

// Skip ranges larger than 4 GB. This filters out sanitizer shadow
// regions (ASAN maps ~128 TB) while keeping all real application
// mappings (code, heap, stack, mmap'd files).
static constexpr uint64_t kMaxRangeBytes = 4ULL * 1024 * 1024 * 1024;

// Read pagemap entries in batches of 512 (4 KB per pread).
static constexpr size_t kBatchPages = 512;

static void dump_pagemap(const char* outdir, const char* filename) {
  auto ranges = parse_self_maps();
  if (ranges.empty()) {
    LOG(WARNING) << "Failed to parse /proc/self/maps, skipping pagemap dump";
    return;
  }

  int pm_fd = open("/proc/self/pagemap", O_RDONLY);
  if (pm_fd < 0) {
    LOG(WARNING) << "Cannot open /proc/self/pagemap (need CAP_SYS_ADMIN), "
                 << "skipping pagemap dump";
    return;
  }

  std::string dir = std::string(outdir) + "/v2p_maps";
  mkdir(dir.c_str(), 0755);

  std::string path = dir + "/" + filename;
  FILE* out = fopen(path.c_str(), "wb");
  if (!out) {
    LOG(WARNING) << "Cannot create " << path << ", skipping pagemap dump";
    close(pm_fd);
    return;
  }

  uint64_t pages_written = 0;
  uint64_t ranges_skipped = 0;
  bool has_nonzero_pfn = false;
  for (const auto& range : ranges) {
    uint64_t range_size = range.end - range.start;
    if (range_size > kMaxRangeBytes) {
      ranges_skipped++;
      continue;
    }

    uint64_t addr = range.start & ~(kPageSize - 1);
    while (addr < range.end) {
      uint64_t remaining = (range.end - addr) / kPageSize;
      size_t batch = static_cast<size_t>(
          remaining < kBatchPages ? remaining : kBatchPages);
      if (batch == 0) {
        break;
      }

      uint64_t pm_offset = (addr / kPageSize) * kPagemapEntrySize;
      uint64_t buf[kBatchPages];
      ssize_t n = pread(pm_fd, buf, batch * kPagemapEntrySize, pm_offset);
      if (n <= 0) {
        break;
      }
      size_t entries_read = n / kPagemapEntrySize;
      for (size_t i = 0; i < entries_read; i++) {
        if (buf[i] & kPresentBit) {
          uint64_t page_addr = addr + i * kPageSize;
          fwrite(&page_addr, sizeof(page_addr), 1, out);
          fwrite(&buf[i], sizeof(buf[i]), 1, out);
          pages_written++;
          if ((buf[i] & kPfnMask) != 0) {
            has_nonzero_pfn = true;
          }
        }
      }
      addr += entries_read * kPageSize;
    }
  }

  fclose(out);
  close(pm_fd);

  if (ranges_skipped > 0) {
    LOG(INFO) << "Skipped " << ranges_skipped
              << " memory ranges >4GB (sanitizer shadow regions)";
  }

  if (pages_written == 0 || !has_nonzero_pfn) {
    // Either no present pages, or PFNs are zeroed (Linux 4.0+ without
    // CAP_SYS_ADMIN). Remove file and directory so absence signals no data.
    unlink(path.c_str());
    rmdir(dir.c_str());
    LOG(WARNING) << "Pagemap not usable (need CAP_SYS_ADMIN for PFNs), "
                 << "no pagemap saved";
  } else {
    LOG(INFO) << "Wrote " << pages_written << " pagemap entries to " << path;
  }
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

  if (g_config.record_pagemap) {
    dump_pagemap(g_config.outdir, "pagemap_pre_trace.bin");
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
  if (g_config.record_pagemap) {
    dump_pagemap(g_config.outdir, "pagemap_post_trace.bin");
  }
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
      if (g_config.record_pagemap) {
        dump_pagemap(g_config.outdir, "pagemap_post_trace.bin");
      }
    }
  });
  // NOLINTNEXTLINE(facebook-hte-BadCall-detach)
  watchdog.detach();
}

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

/**
 * dr_trace_system_info.cpp - Side-channel file dumping for dr_trace.
 *
 * Dumps pagemap (VA->PA), CPU topology, and memory type range files
 * alongside DynamoRIO traces. See dr_trace_system_info.h for details.
 */

#include "dr_trace_system_info.h"

#include <fcntl.h>
#include <glog/logging.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* ---- Pagemap dump ---- */

// Binary format written to pagemap_{pre,post}_trace.bin:
//   16-byte header: [uint64_t magic, uint64_t page_size]
//   Repeated 16-byte entries:
//     uint64_t virt_page_addr   (page-aligned virtual address)
//     uint64_t pagemap_entry    (raw /proc/self/pagemap entry)
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

static constexpr uint64_t kPagemapEntrySize = 8;
static constexpr uint64_t kPresentBit = 1ULL << 63;
static constexpr uint64_t kPfnMask = 0x007FFFFFFFFFFFFFULL;
static constexpr uint64_t kPagemapBinMagic = 0x50474D5000000001ULL;

// Skip ranges larger than 4 GB. This filters out sanitizer shadow
// regions (ASAN maps ~128 TB) while keeping all real application
// mappings (code, heap, stack, mmap'd files).
static constexpr uint64_t kMaxRangeBytes = 4ULL * 1024 * 1024 * 1024;

// Read pagemap entries in batches of 512 (4 KB per pread).
static constexpr size_t kBatchPages = 512;

void dump_pagemap(const char* outdir, const char* filename) {
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

  const uint64_t page_size = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));

  // Write binary header: magic number + page size.
  bool write_ok =
      fwrite(&kPagemapBinMagic, sizeof(kPagemapBinMagic), 1, out) == 1 &&
      fwrite(&page_size, sizeof(page_size), 1, out) == 1;
  if (!write_ok) {
    LOG(WARNING) << "Failed to write pagemap header to " << path;
    fclose(out);
    close(pm_fd);
    unlink(path.c_str());
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

    uint64_t addr = range.start & ~(page_size - 1);
    while (addr < range.end) {
      uint64_t remaining = (range.end - addr) / page_size;
      size_t batch = static_cast<size_t>(
          remaining < kBatchPages ? remaining : kBatchPages);
      if (batch == 0) {
        break;
      }

      uint64_t pm_offset = (addr / page_size) * kPagemapEntrySize;
      uint64_t buf[kBatchPages];
      ssize_t n = pread(pm_fd, buf, batch * kPagemapEntrySize, pm_offset);
      if (n <= 0) {
        break;
      }
      size_t entries_read = n / kPagemapEntrySize;
      for (size_t i = 0; i < entries_read; i++) {
        if (buf[i] & kPresentBit) {
          uint64_t page_addr = addr + i * page_size;
          if (fwrite(&page_addr, sizeof(page_addr), 1, out) != 1 ||
              fwrite(&buf[i], sizeof(buf[i]), 1, out) != 1) {
            LOG(WARNING) << "Write failed after " << pages_written
                         << " pagemap entries, file may be truncated";
            goto done;
          }
          pages_written++;
          if ((buf[i] & kPfnMask) != 0) {
            has_nonzero_pfn = true;
          }
        }
      }
      addr += entries_read * page_size;
    }
  }
done:

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

/* ---- System info dump ---- */

// Strip trailing newline/carriage-return from sysfs reads.
static void strip_newline(char* s) {
  size_t len = strlen(s);
  while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r')) {
    s[--len] = '\0';
  }
}

// Parse a CPU range list like "0-47,96-143" into individual CPU IDs.
static void parse_cpu_range_list(const char* s, std::vector<int>& out) {
  const char* p = s;
  while (*p) {
    int start = 0;
    int end = 0;
    int n = 0;
    if (sscanf(p, "%d-%d%n", &start, &end, &n) == 2) {
      for (int i = start; i <= end; i++) {
        out.push_back(i);
      }
      p += n;
    } else if (sscanf(p, "%d%n", &start, &n) == 1) {
      out.push_back(start);
      p += n;
    } else {
      break;
    }
    if (*p == ',') {
      p++;
    }
  }
}

// Read a small sysfs file into buf. Returns false on failure.
static bool read_sysfs_file(const char* path, char* buf, size_t buf_size) {
  FILE* f = fopen(path, "r");
  if (!f) {
    return false;
  }
  buf[0] = '\0';
  if (fgets(buf, static_cast<int>(buf_size), f)) {
    strip_newline(buf);
  }
  fclose(f);
  return buf[0] != '\0';
}

static void dump_cpu_topology(const char* outdir) {
  std::string dir = std::string(outdir) + "/system_info";
  mkdir(dir.c_str(), 0755);

  // Read online CPUs.
  char online_buf[1024];
  if (!read_sysfs_file(
          "/sys/devices/system/cpu/online", online_buf, sizeof(online_buf))) {
    LOG(WARNING) << "Cannot read /sys/devices/system/cpu/online";
    return;
  }

  std::vector<int> cpus;
  parse_cpu_range_list(online_buf, cpus);
  if (cpus.empty()) {
    LOG(WARNING) << "No online CPUs found";
    return;
  }

  std::string path = dir + "/cpu_topology.csv";
  FILE* out = fopen(path.c_str(), "w");
  if (!out) {
    LOG(WARNING) << "Cannot create " << path;
    return;
  }

  fprintf(out, "cpu,core_id,physical_package_id,thread_siblings_list\n");

  auto read_topology_field =
      [](int cpu, const char* field, const char* fallback) -> std::string {
    char path[256];
    char val[256];
    snprintf(
        path,
        sizeof(path),
        "/sys/devices/system/cpu/cpu%d/topology/%s",
        cpu,
        field);
    return read_sysfs_file(path, val, sizeof(val)) ? val : fallback;
  };

  for (int cpu : cpus) {
    std::string core_id = read_topology_field(cpu, "core_id", "-1");
    std::string pkg_id = read_topology_field(cpu, "physical_package_id", "-1");
    std::string siblings =
        read_topology_field(cpu, "thread_siblings_list", "?");

    fprintf(
        out,
        "%d,%s,%s,\"%s\"\n",
        cpu,
        core_id.c_str(),
        pkg_id.c_str(),
        siblings.c_str());
  }

  fclose(out);
  LOG(INFO) << "Wrote CPU topology for " << cpus.size() << " CPUs to " << path;
}

// Copy a system file verbatim into the system_info directory.
// Returns true if copied, false if source not available (expected on some
// platforms, e.g. /proc/mtrr on ARM).
static bool dump_sysfile_copy(
    const char* outdir,
    const char* src_path,
    const char* dest_name) {
  FILE* src = fopen(src_path, "r");
  if (!src) {
    LOG(INFO) << src_path << " not available, skipping";
    return false;
  }

  std::string dir = std::string(outdir) + "/system_info";
  mkdir(dir.c_str(), 0755);

  std::string dest_path = dir + "/" + dest_name;
  FILE* dst = fopen(dest_path.c_str(), "w");
  if (!dst) {
    LOG(WARNING) << "Cannot create " << dest_path;
    fclose(src);
    return false;
  }

  char buf[4096];
  size_t n;
  bool write_ok = true;
  while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
    if (fwrite(buf, 1, n, dst) != n) {
      LOG(WARNING) << "Write failed copying " << src_path << " to " << dest_path
                   << ", file may be truncated";
      write_ok = false;
      break;
    }
  }

  fclose(src);
  fclose(dst);
  if (write_ok) {
    LOG(INFO) << "Copied " << src_path << " to " << dest_path;
  }
  return write_ok;
}

void dump_system_info(const char* outdir) {
  dump_cpu_topology(outdir);
  dump_sysfile_copy(outdir, "/proc/mtrr", "mtrr.txt");
  dump_sysfile_copy(
      outdir, "/sys/kernel/debug/x86/pat_memtype_list", "pat_memtype_list.txt");
  dump_sysfile_copy(outdir, "/proc/iomem", "iomem.txt");
}

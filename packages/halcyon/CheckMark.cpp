// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.
/*
Checksum benchmark th does strided 4K checksums
in 8M chunks at random 4K aligned offsets
*/

#include <fcntl.h>
#include <unistd.h>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <future>
#include <limits>
#include <random>
#include <vector>

#ifdef __x86_64__
#define crc8 __builtin_ia32_crc32qi
#define crc64 __builtin_ia32_crc32di
#endif

#ifdef __aarch64__
#include <arm_acle.h>
#define crc8 __crc32cb
#define crc64 __crc32cd
#endif

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

using namespace std;

const uint64_t kBufferSize = 100 * 1024 * 1024;
const int kChecksumSize = 4096;
const int kChunkSize = 8 * 1024 * 1024;
const int kPointerChainLength = 10000;

namespace {

struct ChecksumStats {
  uint64_t count = 0;
  uint64_t usec = 0;
  double getLatency() {
    return count > 0 ? usec / static_cast<double>(count) : 0;
  }
};

struct WorkerStats {
  uint64_t startTime = 0;
  uint64_t endTime = 0;
  uint64_t chunkOps = 0;
  ChecksumStats cStats;
  uint64_t getElapsed() {
    return endTime - startTime;
  }
  double getChunkRate() {
    double seconds = (endTime - startTime) / 1e9;
    return seconds > 0 ? chunkOps / seconds : 0;
  }
  double getChunkLatency() {
    return chunkOps > 0 ? cStats.usec / static_cast<double>(chunkOps) : 0;
  }
  double getChecksumLatency() {
    return cStats.getLatency();
  }
};

class Worker {
 public:
  int workerID;
  int runtime;
  int checksumSize;
  int chunkSize;
  Worker(int id, int seconds, int checksumsize, int chunksize);
  WorkerStats start(int thinktime);

 private:
  void* buffer;
  vector<uint64_t> pointerChase;
};

} // namespace

uint64_t current_nano() {
  uint64_t nanos = chrono::duration_cast<chrono::nanoseconds>(
                       chrono::system_clock::now().time_since_epoch())
                       .count();
  return nanos;
}

uint32_t checksum(const uint8_t* buf, size_t size) {
  uint32_t crc = 0;
  const uint8_t* index = buf;
  const uint8_t* last = buf + size;
  const size_t align = sizeof(uint64_t);
  // byte wise until aligned, byte wise for non-aligned tail
  while ((uint64_t)index % align != 0 && index < last) {
    crc = crc8(crc, *index);
    ++index;
  }
  uint64_t alignedChunks = align * ((uint64_t)last / align);
  while ((uint64_t)index < alignedChunks) {
    crc = crc64(crc, *(uint64_t*)index);
    index += align;
  }
  while (index < last) {
    crc = crc8(crc, *index);
    ++index;
  }
  return crc;
}

ChecksumStats
chunkChecksum(const uint8_t* buf, int checksumSize, int chunkSize) {
  int bytesToChecksum = chunkSize;
  int offset = 0;
  ChecksumStats stats;
  uint64_t now = current_nano();
  while (bytesToChecksum > 0) {
    int dataLen = MIN(bytesToChecksum, checksumSize);
    checksum(buf + (offset * checksumSize), dataLen);
    offset++;
    stats.count++;
    bytesToChecksum -= dataLen;
  }
  stats.usec = (current_nano() - now) / 1000;
  return stats;
}

Worker::Worker(int id, int seconds, int checksumsize, int chunksize) {
  workerID = id;
  runtime = seconds;
  checksumSize = checksumsize;
  chunkSize = chunksize;
  posix_memalign(&buffer, checksumSize, kBufferSize);
  if (buffer == nullptr) {
    throw bad_alloc();
  }
  int fd = open("/dev/urandom", O_RDONLY);
  read(fd, buffer, kBufferSize);
  close(fd);
  uint64_t slots = lround((kBufferSize - chunkSize) / checksumSize);
  random_device rd;
  mt19937 mt(rd());
  uniform_int_distribution<uint64_t> randSlot(0, slots);
  for (int i = 0; i < kPointerChainLength; i++) {
    pointerChase.push_back(randSlot(mt));
  }
}

WorkerStats Worker::start(int thinktime) {
  uint64_t startTime = current_nano();
  uint64_t ts = startTime;
  double seconds = (ts - startTime) / 1e9;
  int i = 0;
  WorkerStats stats;
  stats.startTime = startTime;
  assert(!pointerChase.empty());
  while (seconds < runtime) {
    uint64_t offset = pointerChase[i % kPointerChainLength];
    ChecksumStats cstats =
        chunkChecksum((uint8_t*)buffer + offset, checksumSize, chunkSize);
    i++;
    stats.chunkOps++;
    stats.cStats.count += cstats.count;
    stats.cStats.usec += cstats.usec;
    if (thinktime > 0) {
      std::this_thread::sleep_for(std::chrono::microseconds(thinktime));
    }
    ts = current_nano();
    seconds = (ts - startTime) / 1e9;
  }
  stats.endTime = current_nano();
  free(buffer);
  return stats;
}

WorkerStats launchWorker(
    int id,
    int runtime,
    int thinktime,
    int checksumSize,
    int chunkSize) {
  Worker worker(id, runtime, checksumSize, chunkSize);
  return worker.start(thinktime);
}

void printUsage(int returnCode) {
  printf("Usage: checkmark [-n threads][-d duration][-t thinktime]");
  printf("[-s checksum_size][-c chunk_size][-q][-h]\n");
  printf(
      "%20s : %-50s\n",
      "-n threads",
      "number of concurrent threads (default: 1)");
  printf(
      "%20s : %-50s\n",
      "-d duration",
      "duration of the benchmark in seconds (default: 60)");
  printf(
      "%20s : %-50s\n",
      "-t thinktime",
      "thinktime in microseconds between chunk operations (default: 0)");
  printf(
      "%20s : %-50s\n",
      "-s checksum_size",
      "Checksum size in KiB (default: 4)");
  printf(
      "%20s : %-50s\n", "-s chunk_size", "Chunk size in KiB (default: 8192)");
  printf(
      "%20s : %-50s\n",
      "-q",
      "quiet mode. All results printed in one CSV line");
  printf(
      "%20s  %-50s\n",
      "",
      " Output fields: threads,duration,thinktime,elapsed_time,chunk_ops,");
  printf(
      "%20s  %-50s\n",
      "",
      " total_checksums,checksum_ms,chunk_rate,chunk_latency");
  printf("%20s : %-50s\n", "-h", "help message (this)");
  exit(returnCode);
}

int main(int argc, char** argv) {
  int c;
  int runtime = 60;
  int procs = 1;
  int thinktime = 0;
  int checksumSize = kChecksumSize;
  int chunkSize = kChunkSize;
  bool quiet = false;
  while ((c = getopt(argc, argv, "hqn:d:t:s:c:")) != -1) {
    switch (c) {
      case 'n':
        if (optarg) {
          procs = atoi(optarg);
        }
        break;
      case 'd':
        if (optarg) {
          runtime = atoi(optarg);
        }
        break;
      case 't':
        if (optarg) {
          thinktime = atoi(optarg);
        }
        break;
      case 'q':
        quiet = true;
        break;
      case 's':
        if (optarg) {
          checksumSize = 1024 * atoi(optarg);
        }
        break;
      case 'c':
        if (optarg) {
          chunkSize = 1024 * atoi(optarg);
        }
        break;
      case 'h':
        printUsage(0);
        break;
      default:
        printUsage(1);
        break;
    }
  }
  // Validate command line arguments have acceptable values
  if (procs < 1) {
    printf("Number of threads must be positive\n");
    exit(1);
  }
  if (runtime < 1) {
    printf("Duration must be positive.  Units are in seconds\n");
    exit(1);
  }
  if (thinktime < 0) {
    printf("Think time cannot be negative.  Units are in microseconds\n");
    exit(1);
  }
  if (checksumSize < 1024 || checksumSize > kBufferSize) {
    printf(
        "Checksum size must be between 1 and %lu.  Units are in KiB (1024)\n",
        kBufferSize / 1024);
    exit(1);
  }
  if (chunkSize < 1024 || chunkSize > kBufferSize) {
    printf(
        "Chunk size must be between 1 and %lu.  Units are in KiB (1024)\n",
        kBufferSize / 1024);
    exit(1);
  }
  if (chunkSize < checksumSize) {
    printf("Chunk size must be greater than or equal to checksum size.\n");
    exit(1);
  }
  if (!quiet) {
    printf("CheckMark v0.1\n");
    time_t benchmarkStart = time(nullptr);
    array<char, 50> ctime_str;
    printf(
        "Benchmark started at: %s", ctime_r(&benchmarkStart, ctime_str.data()));
    printf("Benchmark parameters:\n");
    printf("--------------------\n");
    printf("  Threads: %d\n", procs);
    printf("  Duration (seconds): %d\n", runtime);
    printf("  Checksum Size (bytes): %d\n", checksumSize);
    printf("  Chunk Size (bytes): %d\n", chunkSize);
    if (thinktime > 0) {
      printf("  Thinktime (microseconds): %d\n", thinktime);
    }
  }
  vector<future<WorkerStats>> futs;
  for (int i = 0; i < procs; i++) {
    futs.push_back(
        std::async(
            std::launch::async,
            &launchWorker,
            i,
            runtime,
            thinktime,
            checksumSize,
            chunkSize));
  }
  WorkerStats totalStats;
  totalStats.startTime = numeric_limits<uint64_t>::max();
  for (auto& s : futs) {
    WorkerStats stats = s.get();
    if (stats.startTime < totalStats.startTime) {
      totalStats.startTime = stats.startTime;
    }
    if (stats.endTime > totalStats.endTime) {
      totalStats.endTime = stats.endTime;
    }
    totalStats.chunkOps += stats.chunkOps;
    totalStats.cStats.count += stats.cStats.count;
    totalStats.cStats.usec += stats.cStats.usec;
  }
  double elapsed = totalStats.getElapsed() / 1e9;
  double rate = totalStats.getChunkRate();
  double lat = totalStats.getChunkLatency();
  if (!quiet) {
    time_t benchmarkEnd = time(nullptr);
    array<char, 50> ctime_str;
    printf(
        "Benchmark completed at: %s", ctime_r(&benchmarkEnd, ctime_str.data()));
    printf("Benchmark results:\n");
    printf("-----------------\n");
    printf("  Elapsed Time (seconds): %.2f\n", elapsed);
    printf("  Chunk operations: %lu\n", totalStats.chunkOps);
    printf("  Total checksums: %lu\n", totalStats.cStats.count);
    printf(
        "  Time spend doing checksums (milliseconds): %.3f\n",
        totalStats.cStats.usec / 1e3);
    printf("  Throughput (chunks/second): %.3f\n", rate);
    printf("  Average Latency (usec/chunk): %.3f\n", lat);
  } else {
    printf(
        "%d,%d,%d,%d,%d,%.2f,%lu,%lu,%.3f,%.3f,%.3f\n",
        procs,
        runtime,
        thinktime,
        checksumSize,
        chunkSize,
        elapsed,
        totalStats.chunkOps,
        totalStats.cStats.count,
        totalStats.cStats.usec / 1e3,
        rate,
        lat);
  }
  return 0;
}

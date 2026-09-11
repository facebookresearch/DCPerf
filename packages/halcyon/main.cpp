// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include <folly/init/Init.h>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include "BenchmarkConfiguration.h"
#include "CpuMonitor.h"
#include "DiskMonitor.h"
#include "Fileset.h"
#include "IoUringEngine.h"
#include "PerfStats.h"
#include "ThreadPools.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <folly/Format.h>
#include <folly/ScopeGuard.h>
#include <folly/String.h>
#include <folly/futures/Future.h>

using namespace facebook;
using namespace facebook::halcyon;
using namespace std;

atomic<bool> pleaseStop = false;

static bool validateRuntime(const char* /* name */, int32_t value) {
  if (value > 0 && value < 5184000) {
    return true;
  }
  printf("Runtime/warmup must be positive and less than 60 days\n");
  return false;
}
static bool validateConfigFile(const char* /* name */, const string& value) {
  if (fileExistsAndIsReadable(value)) {
    return true;
  }
  printf("File does not exist or is not reable: %s\n", value.c_str());
  return false;
}
static bool validateZeroOrPositive(const char* /* name */, double value) {
  if (value >= 0) {
    return true;
  }
  printf("Value can't be negative (0=untrottled)\n");
  return false;
}
static bool validateUnitInterval(const char* /* name */, double value) {
  if (value >= 0.0 && value <= 1.0) {
    return true;
  }
  printf("Value must be a probability in [0, 1]\n");
  return false;
}
static bool validatePositive(const char* /* name */, int32_t value) {
  if (value > 0) {
    return true;
  }
  printf("Value must be positive\n");
  return false;
}
static bool validateNumMountPoints(const char* /* name */, int32_t value) {
  if (value >= 0) {
    return true;
  }
  printf("Mount points can't be negative (0=use all)\n");
  return false;
}
static bool validateReportingInterval(const char* /* name */, int32_t value) {
  if (value >= 0) {
    return true;
  }
  printf("Reporting interval can't be negative (0=don't report)\n");
  return false;
}
static bool validateZeroOrPositiveInt(const char* /* name */, int32_t value) {
  if (value >= 0) {
    return true;
  }
  printf("Value can't be negative (0=auto)\n");
  return false;
}
static bool validateMetadataBackend(
    const char* /* name */,
    const string& value) {
  if (value == "manifest" || value == "rocksdb") {
    return true;
  }
  printf("metadata_backend must be 'manifest' or 'rocksdb'\n");
  return false;
}

DEFINE_int32(runtime, 60, "Runtime in seconds");
DEFINE_validator(runtime, &validateRuntime);
DEFINE_int32(warmup, 60, "Warmup in seconds");
DEFINE_validator(warmup, &validateRuntime);
DEFINE_string(config_file, "config.json", "Configuration file");
DEFINE_validator(config_file, &validateConfigFile);
DEFINE_string(output_file, "halcyon.txt", "Output file");
DEFINE_double(qps, 0, "Overall QPS (putChunk + getChunk)");
DEFINE_validator(qps, &validateZeroOrPositive);
DEFINE_int32(threads_per_disk, 5, "Number of threads per disk");
DEFINE_validator(threads_per_disk, &validatePositive);
DEFINE_int32(num_mountpoints, 0, "Number of mountpoints to use");
DEFINE_validator(num_mountpoints, &validateNumMountPoints);
DEFINE_int32(
    reporting_interval,
    0,
    "Report high level performance periodically (seconds)");
DEFINE_validator(reporting_interval, &validateReportingInterval);
DEFINE_bool(create_fileset, false, "Create fileset");
DEFINE_int32(dirs, 100, "Number of directories per mount point");
DEFINE_validator(dirs, &validatePositive);
DEFINE_int32(files_per_dir, 100, "Number of files per directory");
DEFINE_validator(files_per_dir, &validatePositive);
DEFINE_bool(pin_threads, false, "Pin pool threads to cores from --qio_cores");
DEFINE_string(
    qio_cores,
    "",
    "Comma-separated core list for the QIOThread pool (only used with "
    "--pin_threads)");
DEFINE_int32(
    qio_pool_size,
    0,
    "QIOThread pool size (0 = auto: threads_per_disk * mountpoints)");
DEFINE_validator(qio_pool_size, &validateZeroOrPositiveInt);
DEFINE_int32(
    qflush_pool_size,
    0,
    "QFlush write-offload pool size (rocksdb backend only; 0 = inline puts on "
    "the reactor thread, no offload)");
DEFINE_validator(qflush_pool_size, &validateZeroOrPositiveInt);
DEFINE_string(
    qflush_cores,
    "",
    "Comma-separated core list for the QFlush pool (only used with "
    "--pin_threads)");
DEFINE_string(
    metadata_backend,
    "manifest",
    "I/O addressing backend: manifest (flat dirs/files grid) or rocksdb "
    "(chunk-mapper). Pass the same value to --create_fileset and the run.");
DEFINE_validator(metadata_backend, &validateMetadataBackend);
DEFINE_bool(
    cache_enabled,
    false,
    "Enable the per-reactor ChunkMetadataCache read cache (rocksdb backend "
    "only). Sized by --cache_capacity.");
DEFINE_int32(
    cache_capacity,
    100000,
    "ChunkMetadataCache capacity in entries per reactor (only used with "
    "--cache_enabled)");
DEFINE_validator(cache_capacity, &validatePositive);
DEFINE_double(
    cache_locality,
    0.0,
    "Read-key access locality probability in [0,1] (rocksdb backend). With this "
    "probability a read targets the most-recent --cache_window chunk ids, so "
    "the cache hit rate emerges from reuse; 0 = uniform key selection.");
DEFINE_validator(cache_locality, &validateUnitInterval);
DEFINE_int32(
    cache_window,
    0,
    "Hot-window size in chunk ids for --cache_locality (the most-recently-"
    "allocated ids reads concentrate on); 0 = no locality window.");
DEFINE_validator(cache_window, &validateZeroOrPositiveInt);

void signalHandler(int signum) {
  LOG(INFO) << sformat("Got signal {}", signum);
  pleaseStop = true;
}

void printIntervalHeader() {
  cout << sformat(
      "{0:>7s} {1:>7s} {2:>7s} {3:>7s} {4:>7s} {5:>7s} {6:>11s}\n",
      "Time(s)",
      "QPS",
      "MBpS",
      "Lat(ms)",
      "Disk(%)",
      "CPU(%)",
      "QIO CPU(ms)");
  cout.flush();
}

string printResultsHeader() {
  string result;
  result = sformat(
      "{0:16s} {1:19s} {2:21s} {3:21s} {4:21s} {5:21s} {6:12s} {7:13s}\n",
      "",
      "         QPS       ",
      "         MBpS       ",
      "   Avg Lat (ms)     ",
      "  Chksum / sec      ",
      "Chksum Lat (usec)",
      "CPU Util (%)",
      "Disk Util (%)");
  result += sformat(
      "{0:16s} {1:>8s} {2:>8s} {3:>10s}{4:>10s} {5:>10s}{6:>10s} {7:>10s}{8:>10s}"
      " {9:>10s}{10:>10s}\n",
      "Mount point",
      "Read",
      "Write",
      "Read",
      "Write",
      "Read",
      "Write",
      "Read",
      "Write",
      "Read",
      "Write");
  return result;
}

/// Calculate high level perf stats during benchmark run
IoStats getCurrentIoStats(PerfStats* perfStats) {
  IoStats result;
  for (int i = 0; i < perfStats->nOperators; i++) {
    for (int j = 0; j < perfStats->nWorkers; j++) {
      IoStats k = perfStats->istats.at(i).at(j);
      result.readIos += k.readIos;
      result.writeIos += k.writeIos;
      result.readBytes += k.readBytes;
      result.writeBytes += k.writeBytes;
      result.readUsec += k.readUsec;
      result.writeUsec += k.writeUsec;
    }
  }
  return result;
}

// Maps the validated --metadata_backend flag to the engine enum.
static MetadataBackend parseMetadataBackend(const string& value) {
  return value == "rocksdb" ? MetadataBackend::RocksDb
                            : MetadataBackend::Manifest;
}

void doCreate(
    int threadsPerDisk,
    int dirs,
    int files,
    int reportingInterval,
    int n,
    BenchmarkConfiguration config,
    MetadataBackend backend) {
  uint64_t total_files = dirs * files * n;
  uint64_t total_bytes = total_files * kMaxFileSize;
  vector<std::future<int>> cfuts;
  LOG(INFO) << sformat(
      "Creating fileset: {} files, {} bytes, {} mount points\n",
      prettyPrint(total_files, PRETTY_UNITS_METRIC),
      prettyPrint(total_bytes, PRETTY_BYTES_METRIC),
      n);
  unique_ptr<CreateStats> createStats(new CreateStats(n, threadsPerDisk));
  for (int i = 0; i < n; i++) {
    cfuts.push_back(
        std::async(
            std::launch::async,
            &launchFilesetCreation,
            i,
            threadsPerDisk,
            config.mountPoints.at(i),
            dirs,
            files,
            createStats.get(),
            backend,
            config.readSizes));
  }
  int threadsToComplete = threadsPerDisk * n;
  int threadsComplete = 0;
  uint64_t lastBytes = 0;
  uint64_t lastTime = current_nano();
  uint64_t currBytes = 0;
  int currFiles = 0;
  uint64_t currTime = current_nano();
  uint64_t lastReport = currTime;
  while (threadsComplete < threadsToComplete) {
    if (pleaseStop) {
      LOG(INFO) << "Got interrupt signal, shutting down.";
      createStats->stopNow = true;
    }
    threadsComplete = 0;
    currFiles = 0;
    currBytes = 0;
    for (int i = 0; i < n; i++) {
      for (int j = 0; j < threadsPerDisk; j++) {
        FilesetStats stats = createStats->stats.at(i).at(j);
        currFiles += stats.files;
        currBytes += stats.bytes;
        if (stats.end > 0) {
          threadsComplete++;
        }
      }
    }
    currTime = current_nano();
    double elapsed = (currTime - lastTime) / 1e9;
    bool timeToReport = ((currTime - lastReport) / 1e9) > reportingInterval;
    double mb = (currBytes - lastBytes) / 1e6;
    double mbps = elapsed > 0 ? mb / elapsed : 0;
    double perc = 100 * currFiles / static_cast<double>(total_files);
    if (timeToReport && currFiles > 0) {
      LOG(INFO) << sformat(
          "{0:d}/{1:d} ({2:.2f}%) [{3:.2f} MB/s] files complete",
          currFiles,
          total_files,
          perc,
          mbps);
      lastReport = currTime;
    }
    lastBytes = currBytes;
    lastTime = currTime;
    // needed for reporting partial results while we wait
    // @lint-ignore CLANGTIDY
    std::this_thread::sleep_for(std::chrono::seconds(kCreatePollingSecs));
  }
  int totalErrors = 0;
  for (auto& e : cfuts) {
    totalErrors += e.get();
  }
  if (totalErrors > 0) {
    LOG(ERROR) << sformat("{} errors during fileset creation", totalErrors);
    exit(1);
  } else {
    // write manifest file
    string manifestData = sformat("{} {}\n", dirs, files);
    for (int i = 0; i < n; i++) {
      string fname = sformat("{}/manifest", config.mountPoints.at(i));
      ofstream manifest(fname);
      manifest << manifestData;
      manifest.close();
    }
  }
  LOG(INFO) << "File creation complete\n";
}

static std::vector<int> parseCoreList(const std::string& csv) {
  std::vector<int> cores;
  if (csv.empty()) {
    return cores;
  }
  folly::split(',', csv, cores);
  return cores;
}

void doBenchmark(
    int threadsPerDisk,
    int runtime,
    int warmup,
    double qps,
    int reportingInterval,
    int n,
    BenchmarkConfiguration config,
    string outputFile,
    const PoolConfig& poolConfig,
    MetadataBackend backend) {
  LOG(INFO) << "Starting I/O on " << n << " mount points";
  unique_ptr<PerfStats> perfStats(new PerfStats(n, threadsPerDisk));
  LOG(INFO) << sformat(
      "QIOThread pool size={}, pinning={}",
      poolConfig.qioThreads,
      poolConfig.pinEnabled ? "on" : "off");
  if (poolConfig.qflushThreads > 0) {
    LOG(INFO) << sformat("QFlush pool size={}", poolConfig.qflushThreads);
  }
  HalcyonPools pools = buildPools(poolConfig);

  // Ring must be able to hold all in-flight ops (queue depth ==
  // threadsPerDisk).
  unsigned ringEntries = 256u;
  if (static_cast<unsigned>(threadsPerDisk) > ringEntries) {
    ringEntries = static_cast<unsigned>(threadsPerDisk);
  }
  IoUringEngine::Config engineConfig;
  // Bounds-check before slicing: begin() + n would be undefined behavior if n
  // exceeds the configured mount points (the prior .at()-based path threw).
  CHECK_LE(static_cast<size_t>(n), config.mountPoints.size());
  engineConfig.mountPoints = std::vector<std::string>(
      config.mountPoints.begin(), config.mountPoints.begin() + n);
  engineConfig.warmupSec = warmup;
  engineConfig.runtimeSec = runtime;
  engineConfig.ringEntries = ringEntries;
  engineConfig.threadsPerDisk = threadsPerDisk;
  engineConfig.doDirectIo = true;
  engineConfig.readRatio = config.readPerc;
  engineConfig.iops = (n > 0) ? qps / n : 0.0;
  engineConfig.readSizes = config.readSizes;
  engineConfig.writeSizes = config.writeSizes;
  engineConfig.backend = backend;
  // Offload chunk-map writes to the QFlush pool when one was configured
  // (rocksdb backend only; the engine guards on the backend). Matches the
  // worker count to the pool buildPools created from poolConfig.qflushThreads.
  engineConfig.qflushThreads = static_cast<int>(poolConfig.qflushThreads);
  // Per-reactor read cache + injected read locality (rocksdb backend only; the
  // engine builds the cache and applies locality only for that backend).
  engineConfig.cacheCapacity =
      FLAGS_cache_enabled ? static_cast<size_t>(FLAGS_cache_capacity) : 0;
  engineConfig.readLocality = FLAGS_cache_locality;
  engineConfig.localityWindow = static_cast<ChunkId>(FLAGS_cache_window);
  engineConfig.perfStats = perfStats.get();

  // run() is blocking (it joins its reactor threads), so drive it on a separate
  // thread and let the monitoring loop below sample PerfStats concurrently.
  IoUringEngine engine(pools, std::move(engineConfig));
  std::thread engineThread([&engine] { engine.run(); });
  // Join on every exit path: a throw from the monitoring loop below would
  // otherwise destroy a still-joinable thread and call std::terminate(). The
  // explicit join() on the normal path runs first, so this guard is a no-op
  // there.
  SCOPE_EXIT {
    if (engineThread.joinable()) {
      engineThread.join();
    }
  };

  CpuMonitor cpuMon;
  DiskMonitor diskMon;
  IoStats totalIStats;
  uint64_t startTime = current_nano();
  uint64_t ts = startTime;
  int pollingTime = MAX(kMinBenchmarkPollingSecs, reportingInterval);
  if (pollingTime < reportingInterval) {
    LOG(INFO) << sformat("Setting polling time to {}.", pollingTime);
  }
  int waitTime = runtime + warmup - pollingTime;
  IoStats lastStats;
  uint64_t lastTs = ts;
  uint64_t lastQioCpuNs =
      pools.getCpuNs(PoolRole::Reactor) + pools.getCpuNs(PoolRole::QIOThread);
  double seconds = (ts - startTime) / 1e9;
  while (seconds < waitTime) {
    // do monitoring
    // @lint-ignore CLANGTIDY
    std::this_thread::sleep_for(std::chrono::seconds(pollingTime));
    if (seconds == 0) {
      printIntervalHeader();
    }
    ts = current_nano();
    double elapsed = (ts - lastTs) / 1e9;
    IoStats currStats = getCurrentIoStats(perfStats.get());
    double diskUtil = diskMon.getUtilization();
    double cpuUtil = cpuMon.getUtilization();
    uint64_t ios = currStats.readIos + currStats.writeIos - lastStats.readIos -
        lastStats.writeIos;
    uint64_t bytes = currStats.readBytes + currStats.writeBytes -
        lastStats.readBytes - lastStats.writeBytes;
    uint64_t usec = currStats.readUsec + currStats.writeUsec -
        lastStats.readUsec - lastStats.writeUsec;
    double rate = elapsed > 0 ? ios / elapsed : 0;
    double mbps = elapsed > 0 ? bytes / elapsed / 1e6 : 0;
    double lat = ios > 0 ? usec / ios / 1e3 : 0;
    uint64_t currQioCpuNs =
        pools.getCpuNs(PoolRole::Reactor) + pools.getCpuNs(PoolRole::QIOThread);
    double qioCpuMs = (currQioCpuNs - lastQioCpuNs) / 1e6;
    cout << sformat(
        "{0:>7d} {1:>7.1f} {2:>7.1f} {3:>7.1f} {4:>7.2f} {5:>7.2f} {6:>11.2f}\n",
        static_cast<int>(seconds),
        rate,
        mbps,
        lat,
        diskUtil,
        cpuUtil,
        qioCpuMs);
    cout.flush();
    seconds = (ts - startTime) / 1e9;
    lastStats = currStats;
    lastTs = ts;
    lastQioCpuNs = currQioCpuNs;
  }
  ChecksumStats totalCStats;
  MetadataStats totalMStats;
  double elapsed = static_cast<double>(runtime);
  double readILat = 0.0;
  double writeILat = 0.0;
  double readCLat = 0.0;
  double writeCLat = 0.0;
  // Wait for the engine (runs until warmup+runtime) to finish before reading
  // the final stats.
  engineThread.join();
  ofstream out(outputFile);
  out << printResultsHeader();
  for (int i = 0; i < perfStats->nOperators; i++) {
    IoStats mountIStats;
    ChecksumStats mountCStats;
    for (int j = 0; j < perfStats->nWorkers; j++) {
      IoStats k = perfStats->istats[i][j];
      ChecksumStats l = perfStats->cstats[i][j];
      mountIStats.readIos += k.readIos;
      mountIStats.writeIos += k.writeIos;
      mountIStats.readBytes += k.readBytes;
      mountIStats.writeBytes += k.writeBytes;
      mountIStats.readUsec += k.readUsec;
      mountIStats.writeUsec += k.writeUsec;
      mountIStats.readLatHist.merge(k.readLatHist);
      mountIStats.writeLatHist.merge(k.writeLatHist);
      mountCStats.readCount += l.readCount;
      mountCStats.writeCount += l.writeCount;
      mountCStats.readNanos += l.readNanos;
      mountCStats.writeNanos += l.writeNanos;
      const MetadataStats& m = perfStats->mstats[i][j];
      totalMStats.readCount += m.readCount;
      totalMStats.writeCount += m.writeCount;
      totalMStats.readNanos += m.readNanos;
      totalMStats.writeNanos += m.writeNanos;
      totalMStats.readHits += m.readHits;
      totalMStats.readMisses += m.readMisses;
    }
    readILat = (mountIStats.readIos > 0)
        ? mountIStats.readUsec / 1e3 / mountIStats.readIos
        : 0;
    writeILat = (mountIStats.writeIos > 0)
        ? mountIStats.writeUsec / 1e3 / mountIStats.writeIos
        : 0;
    readCLat = (mountCStats.readCount > 0)
        ? mountCStats.readNanos / 1e3 / mountCStats.readCount
        : 0;
    writeCLat = (mountCStats.writeCount > 0)
        ? mountCStats.writeNanos / 1e3 / mountCStats.writeCount
        : 0;
    out << sformat(
        "{0:16s} {1:8.1f} {2:8.1f} {3:10.2f} {4:10.2f} {5:10.2f}"
        " {6:10.2f} {7:10.2f} {8:10.2f} {9:10.2f} {10:10.2f}\n",
        config.mountPoints.at(i),
        mountIStats.readIos / elapsed,
        mountIStats.writeIos / elapsed,
        mountIStats.readBytes / 1e6 / elapsed,
        mountIStats.writeBytes / 1e6 / elapsed,
        readILat,
        writeILat,
        mountCStats.readCount / elapsed,
        mountCStats.writeCount / elapsed,
        readCLat,
        writeCLat);
    totalIStats.readIos += mountIStats.readIos;
    totalIStats.writeIos += mountIStats.writeIos;
    totalIStats.readBytes += mountIStats.readBytes;
    totalIStats.writeBytes += mountIStats.writeBytes;
    totalIStats.readUsec += mountIStats.readUsec;
    totalIStats.writeUsec += mountIStats.writeUsec;
    totalIStats.readLatHist.merge(mountIStats.readLatHist);
    totalIStats.writeLatHist.merge(mountIStats.writeLatHist);
    totalCStats.readCount += mountCStats.readCount;
    totalCStats.writeCount += mountCStats.writeCount;
    totalCStats.readNanos += mountCStats.readNanos;
    totalCStats.writeNanos += mountCStats.writeNanos;
  }
  readILat = (totalIStats.readIos > 0)
      ? totalIStats.readUsec / 1e3 / totalIStats.readIos
      : 0;
  writeILat = (totalIStats.writeIos > 0)
      ? totalIStats.writeUsec / 1e3 / totalIStats.writeIos
      : 0;
  readCLat = (totalCStats.readCount > 0)
      ? totalCStats.readNanos / 1e3 / totalCStats.readCount
      : 0;
  writeCLat = (totalCStats.writeCount > 0)
      ? totalCStats.writeNanos / 1e3 / totalCStats.writeCount
      : 0;
  double diskUtil = diskMon.getUtilizationFromBeginning();
  double cpuUtil = cpuMon.getUtilizationFromBeginning();
  out << sformat(
      "{0:16s} {1:8.1f} {2:8.1f} {3:10.2f} {4:10.2f} {5:10.2f}"
      " {6:10.2f} {7:10.2f} {8:10.2f} {9:10.2f} {10:10.2f}"
      " {11:12.2f} {12:13.2f}\n",
      "Total",
      totalIStats.readIos / elapsed,
      totalIStats.writeIos / elapsed,
      totalIStats.readBytes / 1e6 / elapsed,
      totalIStats.writeBytes / 1e6 / elapsed,
      readILat,
      writeILat,
      totalCStats.readCount / elapsed,
      totalCStats.writeCount / elapsed,
      readCLat,
      writeCLat,
      cpuUtil,
      diskUtil);
  double qioTotalCpuMs = (pools.getCpuNs(PoolRole::Reactor) +
                          pools.getCpuNs(PoolRole::QIOThread)) /
      1e6;
  double qioChecksumCpuMs =
      (totalCStats.readNanos + totalCStats.writeNanos) / 1e6;
  // QFlush write-offload pool CPU: when --qflush_pool_size > 0 (rocksdb
  // backend) the chunk-map put + WAL-sync cost shows here instead of inline
  // write_puts.
  double qflushTotalCpuMs = pools.getCpuNs(PoolRole::QFlush) / 1e6;
  out << sformat(
      "\nPer-pool CPU (total): qio_total_cpu_ms={:.2f}"
      " qio_checksum_cpu_ms={:.2f} qflush_total_cpu_ms={:.2f}\n",
      qioTotalCpuMs,
      qioChecksumCpuMs,
      qflushTotalCpuMs);
  // Metadata path cost (zero for the manifest backend). Per-op averages let the
  // rocksdb backend be compared against manifest directly.
  double readMetaLat = (totalMStats.readCount > 0)
      ? totalMStats.readNanos / 1e3 / totalMStats.readCount
      : 0;
  double writeMetaLat = (totalMStats.writeCount > 0)
      ? totalMStats.writeNanos / 1e3 / totalMStats.writeCount
      : 0;
  out << sformat(
      "Metadata path (total): read_lookups={} (avg {:.2f} usec)"
      " write_puts={} (avg {:.2f} usec)\n",
      totalMStats.readCount,
      readMetaLat,
      totalMStats.writeCount,
      writeMetaLat);
  // ChunkMetadataCache hit rate (rocksdb backend with the cache enabled). When
  // the cache is off, hits/misses stay zero and the rate reads 0.
  double cacheHitRate = (totalMStats.readCount > 0)
      ? 100.0 * totalMStats.readHits / totalMStats.readCount
      : 0;
  out << sformat(
      "Metadata cache (total): read_hits={} read_misses={} hit_rate={:.2f}%\n",
      totalMStats.readHits,
      totalMStats.readMisses,
      cacheHitRate);
  out.close();
  uint64_t totalIos = totalIStats.readIos + totalIStats.writeIos;
  uint64_t totalUsec = totalIStats.readUsec + totalIStats.writeUsec;
  double totalLat = totalIos > 0 ? totalUsec / totalIos / 1e3 : 0;
  cout << sformat(
      "{0:>7s} {1:>7.1f} {2:>7.1f} {3:>7.1f} {4:>7.2f} {5:>7.2f}\n",
      "Total",
      totalIos / elapsed,
      (totalIStats.readBytes + totalIStats.writeBytes) / elapsed / 1e6,
      totalLat,
      diskUtil,
      cpuUtil);
  LOG(INFO) << sformat("Full results in {}", outputFile);
}

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);
  BenchmarkConfiguration config(FLAGS_config_file);
  int numMountPoints = config.numMountPoints();
  int n = numMountPoints;
  if (n == 0) {
    LOG(ERROR) << "Found zero mount points in " << FLAGS_config_file;
  }
  if (FLAGS_num_mountpoints > 0) {
    n = MIN(numMountPoints, FLAGS_num_mountpoints);
  }
  const MetadataBackend backend = parseMetadataBackend(FLAGS_metadata_backend);
  if (FLAGS_create_fileset) {
    doCreate(
        FLAGS_threads_per_disk,
        FLAGS_dirs,
        FLAGS_files_per_dir,
        FLAGS_reporting_interval,
        n,
        config,
        backend);
  } else {
    PoolConfig poolConfig;
    poolConfig.qioThreads = FLAGS_qio_pool_size > 0
        ? static_cast<size_t>(FLAGS_qio_pool_size)
        : static_cast<size_t>(FLAGS_threads_per_disk) * n;
    poolConfig.pinEnabled = FLAGS_pin_threads;
    poolConfig.qioCores = parseCoreList(FLAGS_qio_cores);
    // Reactors have their own factory with a separate round-robin counter.
    // In the standalone binary the operator provides an explicit --qio_cores
    // list; use the same list for reactors so pinning still applies (the
    // two factories will independently spread across it).
    poolConfig.reactorCores = poolConfig.qioCores;
    poolConfig.qflushThreads = FLAGS_qflush_pool_size > 0
        ? static_cast<size_t>(FLAGS_qflush_pool_size)
        : 0;
    poolConfig.qflushCores = parseCoreList(FLAGS_qflush_cores);
    doBenchmark(
        FLAGS_threads_per_disk,
        FLAGS_runtime,
        FLAGS_warmup,
        FLAGS_qps,
        FLAGS_reporting_interval,
        n,
        config,
        FLAGS_output_file,
        poolConfig,
        backend);
  }
  return 0;
}

// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <array>
#include <cstdint>
#include <future>
#include <list>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <folly/Format.h>
#include <folly/stats/Histogram.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

namespace facebook::halcyon {

using namespace std;
using namespace folly;

/// Sector size of the underlying block device
/// Direct I/O must be aligned to this size
const int kSectorSize = 4096;
/// Minimum I/O size. Must be a multiple of the sector size
const int kMinIoSize = 4096;
/// Maximum file size. 1033 * 2^13 as seen in warm storage
/// 4K header/footer,  8 MiB / (4K - 32) fragments
const int kMaxFileSize = 2066 * 4096;
/// Maximum I/O size and size for read/write buffers
const int kBufferSize = 8 * 1024 * 1024;
/// Size of full chunk
const int kChunkSize = 8 * 1024 * 1024;
/// Size of fragment
const int kFragmentSize = 4096;
/// Size of chunk header/footer
const int kChunkHeaderSize = 4096;
/// Maximum size of data fragments
const int kMaxDataFragmentsSize = 2065 * 4096;
/// Size of chunk checksums
const int kChecksumSize = 4096;
/// Number of past I/O to use for rate limiting calculation
const int kLatSleepHistory = 100;
/// Bucket size for latency histograms (microseconds)
const int kLatHistBucketSize = 2000;
/// Maximum latency for histograms (microseconds)
const int kLatHistMaxValue = 200000;
/// Polling time during fileset creation
const int kCreatePollingSecs = 1;
/// Minimum reporting interval during benchmark run
const int kMinBenchmarkPollingSecs = 5;
/// Starting thinktime between I/Os so we don't blast it
const int kInitialThinktimeUsec = 5000;

/// Selects how the engine addresses I/O. Manifest is the legacy flat dirs x
/// files grid with random-fd / random-offset addressing; RocksDb routes every
/// op through a RocksDB chunk-mapper (chunk id -> physical location), putting a
/// metadata lookup (reads) or persist (writes) in the I/O hot path.
enum class MetadataBackend {
  Manifest,
  RocksDb,
};

/// @brief Returns the current epoch time in nanoseconds.
/// @details This function is used for all I/O measurements
uint64_t current_nano();
/// @brief Converts a an capacity string to an integer.
/// e.g. "16K" -> 16384
int humanToInt(string str);
/// @brief Checks that a file exists and is readable
bool fileExistsAndIsReadable(const string& fname);
/// @brief Checks that the input path is a directory
bool directoryExists(const string& fname);

/// Core data structure for counting I/O performance statistics
struct IoStats {
  uint64_t readIos = 0;
  uint64_t writeIos = 0;
  uint64_t readBytes = 0;
  uint64_t writeBytes = 0;
  uint64_t readUsec = 0;
  uint64_t writeUsec = 0;
  // Hypernode-style reactor-loop accounting (mirrors ThreadMgr.cpp:820-836).
  // usefulBusyNs is per-iteration ticks in loops that did real work, minus a
  // rolling-window average of idle-loop overhead. usefulIdleNs is idle-loop
  // ticks plus the overhead portion subtracted from busy loops. Only populated
  // on slot 0 of each reactor (per-reactor accounting, not per-slot). The
  // ratio busy/(busy+idle) reports "useful CPU" independent of the fact that
  // a busy-poll reactor always pegs its core in Linux mpstat.
  uint64_t usefulBusyNs = 0;
  uint64_t usefulIdleNs = 0;
  Histogram<uint64_t> readLatHist =
      Histogram<uint64_t>(kLatHistBucketSize, 0, kLatHistMaxValue);
  Histogram<uint64_t> writeLatHist =
      Histogram<uint64_t>(kLatHistBucketSize, 0, kLatHistMaxValue);
};

/// Performance counters for network operations
struct NetworkStats {
  uint64_t start = 0;
  uint64_t end = 0;
  uint64_t recvIos = 0;
  uint64_t sendIos = 0;
  uint64_t recvBytes = 0;
  uint64_t sendBytes = 0;
  uint64_t recvUsec = 0;
  uint64_t sendUsec = 0;
};

/// Performance counters for checksum operations, timed inline on the reactor.
/// readCount/writeCount are the number of checksum operations performed on the
/// read / write path (the per-buffer checksum-unit count, not bytes or I/O
/// count); readNanos/writeNanos are their CPU time. They drive the
/// "Chksum / sec" and "Chksum Lat (usec)" report columns. The write path
/// checksums the buffer it just wrote -- modeling the CPU cost of computing a
/// checksum to store alongside the data -- mirroring the read path's verify;
/// the checksum itself is a CPU emulation and is never stored or validated.
struct ChecksumStats {
  uint64_t readCount = 0;
  uint64_t writeCount = 0;
  uint64_t readNanos = 0;
  uint64_t writeNanos = 0;
};

/// Performance counters for the RocksDb chunk-mapper metadata path, timed
/// inline on the reactor thread like the checksum. readCount/readNanos count
/// chunk-id lookups on the read path; writeCount/writeNanos count chunk-id ->
/// location puts on the write path. All zero for the Manifest backend (no
/// metadata ops).
struct MetadataStats {
  uint64_t readCount = 0;
  uint64_t writeCount = 0;
  uint64_t readNanos = 0;
  uint64_t writeNanos = 0;
  // ChunkMetadataCache front-end breakdown of readCount (plan #1), populated
  // once the cache is wired into the read path: readCount == readHits +
  // readMisses. A hit skips the DB lookup (only the cache probe is charged to
  // readNanos); a miss pays the full DB read. Zero when the cache is disabled.
  uint64_t readHits = 0;
  uint64_t readMisses = 0;
};

/// Struct for keeping track of fileset creation
struct FilesetStats {
  int files = 0;
  int dirs = 0;
  uint64_t start = 0;
  uint64_t end = 0;
  uint64_t bytes = 0;
};

// Struct containing a single or range of I/O sizes with
// an associated probability percentage
struct IoSizeRange {
  string text;
  uint64_t lowerBound = 0;
  uint64_t upperBound = 0;
  double percentage = 0;
};

int getIoSize(double rnd, vector<IoSizeRange>* ioSizes);
/// Chooses a random size according to the I/O range probabilities
/// for the next I/O
uint64_t randomIoSize(IoSizeRange ioRange);
/// Choose a random aligned offset within a file
uint64_t randomOffset(int ioSize);
/// Calculate the actual I/O size to file/disk based on application I/O size
int getPureIoSize(int ioSize);

/// Converts I/O size range and percentage to an IoSizeRange struct.
/// @param text string parsed from config file (e.g "20K-32K")
/// @param perc percentage associated with this range
/// @return IoSizeRange struct with range or single size data
IoSizeRange parseIoSize(string text, double perc);
/// Emulate fragment checksumming
int doChecksum(uint8_t* data, int size);
} // namespace facebook::halcyon

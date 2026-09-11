// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "Common.h"

#include <sys/stat.h>
#include <unistd.h>

#include <cctype>

#include <folly/Random.h>
#include <folly/String.h>
#include <folly/hash/Checksum.h>

namespace facebook::halcyon {

using namespace std;
using namespace folly;

const array<pair<string, int>, 3> kBinarySuffix = {
    std::make_pair("K", 1024),
    std::make_pair("M", 1024 * 1024),
    std::make_pair("G", 1024 * 1024 * 1024)};

uint64_t current_nano() {
  uint64_t nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  return nanos;
}

int humanToInt(string str) {
  if (str.empty()) {
    return 0;
  }
  int multiplier = 1;
  string b{
      static_cast<char>(std::toupper(static_cast<unsigned char>(str.back())))};
  for (const pair<string, int>& p : kBinarySuffix) {
    if (b.compare(p.first) == 0) {
      multiplier = p.second;
      str.pop_back();
      break;
    }
  }
  int result = stoi(str) * multiplier;
  return result;
}

// Use access(R_OK): O(1) syscall, no fd allocation, no leak.
bool fileExistsAndIsReadable(const string& fname) {
  return ::access(fname.c_str(), R_OK) == 0;
}

bool directoryExists(const string& fname) {
  struct stat statInfo;
  return ::stat(fname.c_str(), &statInfo) == 0 && S_ISDIR(statInfo.st_mode);
}

IoSizeRange parseIoSize(string text, double perc) {
  IoSizeRange result;
  result.text = text;
  result.percentage = perc;
  vector<folly::StringPiece> v;
  folly::split('-', text, v);
  int s = v.size();
  if (s == 1) {
    result.upperBound = humanToInt(v.at(0).toString());
  } else if (s == 2) {
    result.lowerBound = humanToInt(v.at(0).toString());
    result.upperBound = humanToInt(v.at(1).toString());
  }
  if (result.upperBound == 0) {
    LOG(ERROR) << sformat("Got 0 I/O size upper bound for {}", text);
    exit(1);
  }
  return result;
}

int getIoSize(double rnd, vector<IoSizeRange>* ioSizes) {
  if (ioSizes->size() == 1) {
    return randomIoSize(ioSizes->at(0));
  }
  double cdf = 0;
  vector<IoSizeRange>::reverse_iterator it = ioSizes->rbegin();
  while (it != ioSizes->rend() && cdf < rnd) {
    cdf += it->percentage / 100.0;
    it++;
  }
  if (it == ioSizes->rend()) {
    it--;
  }
  return randomIoSize(*it);
}

uint64_t randomIoSize(IoSizeRange ioRange) {
  if (ioRange.lowerBound == 0) {
    return ioRange.upperBound;
  }
  uint64_t rnd = Random::rand64(ioRange.lowerBound, ioRange.upperBound + 1);
  return kMinIoSize * (rnd / kMinIoSize);
}

uint64_t randomOffset(int ioSize) {
  // Guard against a non-positive size: a misconfigured I/O-size distribution
  // can yield ioSize == 0, which would divide-by-zero below (undefined
  // behavior).
  if (ioSize <= 0) {
    return 0;
  }
  if (ioSize >= kMaxFileSize) {
    return 0;
  }
  uint64_t slots = kMaxDataFragmentsSize / ioSize;
  uint64_t offset = kChunkHeaderSize; // advance past header
  if (slots > 1) {
    offset += ioSize * Random::rand64(slots);
  }
  return offset;
}

int getPureIoSize(int ioSize) {
  int fragments = ceil(ioSize / 4064); // fragment = 32 byte footer, 4064 data
  int pureIoSize = kFragmentSize * fragments;
  if (ioSize >= kChunkSize) {
    pureIoSize = kMaxFileSize;
  }
  return pureIoSize;
}

int doChecksum(uint8_t* data, int size) {
  /*
  uint8_t* checksumBuffer = new uint8_t[kBufferSize];
  for (int i=0; i<kBufferSize; ++i) {
    checksumBuffer[i] = Random::rand32() % 256;
  }*/
  // uint32_t result = 0;
  int offset = 0;
  int bytesToChecksum = size;
  while (bytesToChecksum > 0) {
    int dataLen = MIN(bytesToChecksum, kChecksumSize);
    // algo.update(checksumBuffer + (offset * kChecksumSize), dataLen);
    folly::crc32(data + (offset * kChecksumSize), dataLen, 0);
    offset++;
    bytesToChecksum -= dataLen;
  }
  // delete[] checksumBuffer;
  return offset;
}

} // namespace facebook::halcyon

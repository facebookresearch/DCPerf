// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#define _FILE_OFFSET_BITS 64
#define _LARGFILE64_SOURCE

#include "Fileset.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <folly/Random.h>

#include "Common.h" // IoSizeRange, getIoSize, geometry consts
#include "HalcyonChunkMapper.h"

namespace facebook::halcyon {

using namespace std;

/// function that is launched asynchronously to starte fileset creation
int launchFilesetCreation(
    int operatorid,
    int tpd,
    string basedir,
    int dirs,
    int files,
    CreateStats* createStats,
    MetadataBackend backend,
    vector<IoSizeRange> readSizes) {
  Fileset fs(
      operatorid,
      std::move(basedir),
      dirs,
      files,
      tpd,
      backend,
      std::move(readSizes));
  return fs.create(createStats);
}

int createFileset(
    int operatorID,
    int threadID,
    string baseDir,
    int dirs,
    int files,
    int threadsPerDir,
    CreateStats* createStats) {
  int errors = 0;
  void* writeBuffer;
  posix_memalign(&writeBuffer, kMinIoSize, kMaxFileSize);
  if (writeBuffer == nullptr) {
    throw std::bad_alloc();
  }
  int fd = open("/dev/urandom", O_RDONLY);
  read(fd, writeBuffer, kMaxFileSize);
  close(fd);
  createStats->inProgress = true;
  createStats->stats.at(operatorID).at(threadID).start = current_nano();
  for (int i = 0; i < dirs; i++) {
    int d = i * threadsPerDir + threadID;
    string subDir = sformat("{}/d{}", baseDir, d);
    if (!directoryExists(subDir)) {
      if (mkdir(subDir.c_str(), 0755) == -1) {
        LOG(ERROR) << "Error creating " << subDir;
        free(writeBuffer);
        return errors;
      }
    }
    for (int j = 0; j < files; j++) {
      string fname = sformat("{}/f{}", subDir, j);
      fd = open(fname.c_str(), O_CREAT | O_WRONLY | O_DIRECT | O_SYNC, 0644);
      int bytesWritten = 0;
      if ((bytesWritten = write(fd, writeBuffer, kMaxFileSize)) !=
          kMaxFileSize) {
        LOG(ERROR) << sformat(
            "Write error on file {} from thread {}: {} bytes written",
            fname,
            threadID,
            bytesWritten);
        errors++;
      } else {
        createStats->stats.at(operatorID).at(threadID).bytes += bytesWritten;
      }
      close(fd);
      createStats->stats.at(operatorID).at(threadID).files++;
      if (createStats->stopNow) {
        break;
      }
    }
    if (createStats->stopNow) {
      break;
    }
    createStats->stats.at(operatorID).at(threadID).dirs++;
  }
  createStats->stats.at(operatorID).at(threadID).end = current_nano();
  createStats->inProgress = false;
  free(writeBuffer);
  return errors;
}

void populateChunkMap(
    const string& baseDir,
    int dirs,
    int files,
    vector<IoSizeRange> readSizes,
    bool doDirectIo) {
  if (readSizes.empty()) {
    throw std::runtime_error(
        "populateChunkMap requires a non-empty readSizes distribution");
  }

  HalcyonChunkMapper::Options opts;
  opts.doDirectReads = doDirectIo;
  HalcyonChunkMapper mapper(baseDir + "/chunkmap.rocksdb", opts);

  const int numFiles = dirs * files;
  ChunkId nextChunkId = 0;
  for (int fileIndex = 0; fileIndex < numFiles; ++fileIndex) {
    vector<pair<ChunkId, ChunkLocation>> entries;
    // Upper bound: a file holds at most all-minimum-size chunks.
    entries.reserve((kMaxFileSize - kChunkHeaderSize) / kMinIoSize);
    uint64_t offset = kChunkHeaderSize;
    for (;;) {
      const int len = getIoSize(folly::Random::randDouble01(), &readSizes);
      if (len <= 0 ||
          offset + static_cast<uint64_t>(len) >
              static_cast<uint64_t>(kMaxFileSize)) {
        break;
      }
      ChunkLocation loc;
      loc.fileIndex = static_cast<uint32_t>(fileIndex);
      loc.length = static_cast<uint32_t>(len);
      loc.offset = offset;
      entries.emplace_back(nextChunkId++, loc);
      offset += static_cast<uint64_t>(len);
    }
    if (!entries.empty()) {
      mapper.putBatch(entries);
    }
  }
  mapper.setMaxChunkId(nextChunkId);
}

Fileset::Fileset(
    int operatorID,
    string baseDir,
    int dirs,
    int files,
    int threadsPerDir,
    MetadataBackend backend,
    vector<IoSizeRange> readSizes) {
  operatorID_ = operatorID;
  baseDir_ = std::move(baseDir);
  dirs_ = dirs;
  files_ = files;
  threadsPerDir_ = threadsPerDir;
  backend_ = backend;
  readSizes_ = std::move(readSizes);
}

int Fileset::create(CreateStats* createStats) {
  vector<future<int>> createThreads;
  createThreads.reserve(threadsPerDir_);
  for (int t = 0; t < threadsPerDir_; t++) {
    createThreads.push_back(
        std::async(
            std::launch::async,
            &createFileset,
            operatorID_,
            t,
            baseDir_,
            dirs_,
            files_,
            threadsPerDir_,
            createStats));
  }
  int createErrors = 0;
  for (auto& t : createThreads) {
    createErrors += t.get();
  }
  if (createErrors > 0) {
    LOG(INFO) << sformat("{} errors during fileset creation", createErrors);
  } else {
    // write manifest file
    string manifestData = sformat("{} {}\n", dirs_, files_);
    string fname = sformat("{}/manifest", baseDir_);
    ofstream manifest(fname);
    manifest << manifestData;
    manifest.close();

    if (backend_ == MetadataBackend::RocksDb) {
      // The data files are created with O_DIRECT (see createFileset), so the
      // chunk map DB uses direct I/O too. Populate it so the RocksDb read path
      // has chunks to look up.
      populateChunkMap(
          baseDir_, dirs_, files_, readSizes_, /*doDirectIo=*/true);
    }
  }
  return createErrors;
}

} // namespace facebook::halcyon

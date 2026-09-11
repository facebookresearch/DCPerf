// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#pragma once

#include "Common.h" // IoSizeRange, MetadataBackend
#include "CreateStats.h"

#include <string>
#include <vector>

namespace facebook::halcyon {

using namespace std;

int launchFilesetCreation(
    int operatorid,
    int tpd,
    string basedir,
    int dirs,
    int files,
    CreateStats* createStats,
    MetadataBackend backend = MetadataBackend::Manifest,
    vector<IoSizeRange> readSizes = {});

int createFileset(
    int operatorID,
    int threadID,
    string baseDir,
    int dirs,
    int files,
    int threadsPerDir,
    CreateStats* createStats);

/// Populates the RocksDb chunk map for a freshly created fileset so the RocksDb
/// read path has chunks to look up. Lays out chunks back-to-back in each of the
/// dirs*files data files, with sizes sampled from `readSizes` (so a uniformly
/// random chunk read approximates the configured read-size distribution), and
/// records a chunk-id -> location mapping for each. Chunk ids are a contiguous
/// [0, total) space. Throws std::runtime_error on empty `readSizes` or DB open
/// failure.
void populateChunkMap(
    const string& baseDir,
    int dirs,
    int files,
    vector<IoSizeRange> readSizes,
    bool doDirectIo);

class Fileset {
 public:
  int operatorID_;
  string baseDir_;
  int dirs_;
  int files_;
  int threadsPerDir_;
  MetadataBackend backend_;
  vector<IoSizeRange> readSizes_;
  Fileset(
      int operatorID,
      string baseDir,
      int dirs,
      int files,
      int threadsPerDir,
      MetadataBackend backend = MetadataBackend::Manifest,
      vector<IoSizeRange> readSizes = {});
  int create(CreateStats* createStats);
};

} // namespace facebook::halcyon

// Copyright (c) Meta Platforms, Inc. and affiliates.
//
// This source code is licensed under the MIT license found in the
// LICENSE file in the root directory of this source tree.

#include "BenchmarkConfiguration.h"

#include <folly/json/json.h>
#include <fstream>
#include <vector>

namespace facebook::halcyon {

using namespace std;
using namespace folly;

BenchmarkConfiguration::BenchmarkConfiguration(string fname) {
  std::ifstream is;
  string configJson;
  is.open(fname);
  while (!is.eof()) {
    string str;
    getline(is, str);
    configJson.append(str);
  }
  is.close();
  dynamic parsedConfig = parseJson(configJson);
  double readRatio = 0.0;
  double writeRatio = 0.0;
  bool readKey = true;
  bool writeKey = true;
  auto hasReadRatio = parsedConfig.find("read_ratio");
  if (hasReadRatio != parsedConfig.items().end()) {
    readRatio = parsedConfig["read_ratio"].asDouble();
  } else {
    readKey = false;
  }
  auto hasWriteRatio = parsedConfig.find("write_ratio");
  if (hasWriteRatio != parsedConfig.items().end()) {
    writeRatio = parsedConfig["write_ratio"].asDouble();
  } else {
    writeKey = false;
  }
  if (!readKey && !writeKey) {
    printf("Must specify read_ratio or write_ratio.\n");
    exit(1);
  }
  if (readRatio <= 0 && writeRatio <= 0) {
    printf("Invalid read/write ratio combination.\n");
    exit(1);
  }
  readPerc = readRatio / (readRatio + writeRatio);
  readKey = true;
  writeKey = true;
  if (parsedConfig.find("read_sizes") != parsedConfig.items().end()) {
    auto readSizesConfig = parsedConfig["read_sizes"];
    for (auto& key : readSizesConfig.keys()) {
      string k = key.asString();
      double perc = readSizesConfig[k].asDouble();
      if (perc == 0) {
        continue;
      }
      IoSizeRange range = parseIoSize(k, perc);
      readSizes.push_back(range);
    }
  } else {
    readKey = false;
  }
  if (parsedConfig.find("write_sizes") != parsedConfig.items().end()) {
    auto writeSizesConfig = parsedConfig["write_sizes"];
    for (auto& key : writeSizesConfig.keys()) {
      string k = key.asString();
      double perc = writeSizesConfig[k].asDouble();
      if (perc == 0) {
        continue;
      }
      IoSizeRange range = parseIoSize(k, perc);
      writeSizes.push_back(range);
    }
  } else {
    writeKey = false;
  }
  if (!readKey && !writeKey) {
    printf("Must specify read_sizes or write_sizes.\n");
    exit(1);
  }
  if (readSizes.empty() && writeSizes.empty()) {
    printf("No read/write sizes listed.\n");
    exit(1);
  }
  if (readSizes.empty() && readRatio > 0) {
    printf("Found no read sizes for non-zero read_ratio\n");
    exit(1);
  }
  if (writeSizes.empty() && writeRatio > 0) {
    printf("Found no write sizes for non-zero write_ratio\n");
    exit(1);
  }
  auto hasKey = parsedConfig.find("mount_points");
  if (hasKey != parsedConfig.items().end()) {
    auto mounts = parsedConfig["mount_points"];
    for (auto& s : mounts) {
      mountPoints.push_back(s.asString());
    }
  } else {
    printf("Found no mountpoints.\n");
    exit(1);
  }
}

int BenchmarkConfiguration::numMountPoints() {
  return mountPoints.size();
}

} // namespace facebook::halcyon

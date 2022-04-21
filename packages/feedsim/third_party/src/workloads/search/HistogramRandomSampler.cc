// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <cassert>
#include <chrono>
#include <fstream>
#include <sstream>

#include "HistogramRandomSampler.h"

HistogramRandomSampler::HistogramRandomSampler() : count_sum_(0) { InitRNG(); }

HistogramRandomSampler::HistogramRandomSampler(std::string histogram_file)
    : count_sum_(0) {
  // Initialize RNG seed
  InitRNG();

  // Read the file, which is a tab delimited file of
  // START  END COUNT
  std::ifstream input_file(histogram_file.c_str());
  std::string line;

  while (std::getline(input_file, line)) {
    int start, end, count;
    std::istringstream ss(line);
    ss >> start >> end >> count;

    AddBin(start, end, count);
  }
}

void HistogramRandomSampler::AddBin(int start, int end, int count) {
  bins_.push_back(Bin({start, end, count}));
  count_sum_ += count;
}

int HistogramRandomSampler::Sample() {
  int r = rng_() % count_sum_;
  // Locate the bin to use
  for (const auto& bin : bins_) {
    if (r < bin.count) {
      return (rng_() % (bin.end - bin.start)) + bin.start;
    } else {
      r -= bin.count;
    }
  }

  assert(false);
  return -1;
}

void HistogramRandomSampler::InitRNG() {
  unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
  rng_.seed(seed);
}

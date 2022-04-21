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

#ifndef HISTOGRAM_RANDOM_SAMPLER
#define HISTOGRAM_RANDOM_SAMPLER

#include <random>
#include <string>
#include <vector>

class HistogramRandomSampler {
 public:
  HistogramRandomSampler();
  explicit HistogramRandomSampler(std::string histogram_file);
  void AddBin(int start, int end, int count);
  int Sample();

 private:
  void InitRNG();
  struct Bin {
    int start;
    int end;
    int count;
  };
  std::vector<Bin> bins_;
  int count_sum_;
  std::default_random_engine rng_;
};

#endif  // HISTOGRAM_RANDOM_SAMPLER

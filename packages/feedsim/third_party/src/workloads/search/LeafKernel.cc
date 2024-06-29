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

#include <iostream>

#include "oldisim/Util.h"
#include "PointerChase.h"
#include "ICacheBuster.h"

const int kPointerChaseSize = 10000000;
const int kICacheBusterSize = 100000;
const int kNumNops = 6;
const int kNumNopIterations = 60;
const int kNumIterations = 100000000;

int main(int argc, char** argv) {
  // Create pointer chaser
  search::PointerChase chaser(kPointerChaseSize);

  // Create i cache chaser
  ICacheBuster buster(kICacheBusterSize);

  uint64_t start_time = GetTimeAccurateNano();
  for (int i = 0; i < kNumIterations; i++) {
    buster.RunNextMethod();
    chaser.Chase(1);
    for (int j = 0; j < kNumNopIterations; j++) {
      for (int k = 0; k < kNumNops; k++) {
        asm volatile("nop");
      }
    }
  }
  uint64_t end_time = GetTimeAccurateNano();

  std::cout << "Time per iteration: "
            << static_cast<double>(end_time - start_time) /
               (static_cast<double>(kNumIterations))
            << " ns" << std::endl;

  return 0;
}


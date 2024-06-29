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
#include "PointerChaseTestCmdline.h"
#include "PointerChase.h"

static gengetopt_args_info args;

int main(int argc, char** argv) {
  // Parse arguments
  if (cmdline_parser(argc, argv, &args) != 0) {
    DIE("cmdline_parser failed");
  }

  // Create pointer chaser
  search::PointerChase chaser(args.size_arg);

  uint64_t start_time = GetTimeAccurateNano();
  for (int i = 0; i < args.iterations_arg; i++) {
    chaser.Chase(args.length_arg);
  }
  uint64_t end_time = GetTimeAccurateNano();

  std::cout << "Time per access: "
            << static_cast<double>(end_time - start_time) /
               (static_cast<double>(args.iterations_arg) * args.length_arg)
            << " ns" << std::endl;

  return 0;
}


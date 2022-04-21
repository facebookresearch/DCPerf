#!/usr/bin/env python
#
# Copyright 2015 Google Inc. All Rights Reserved.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Module to split file into multiple segments."""

import argparse
import itertools

HEADER_TEMPLATE = """
#ifndef ICACHE_BUSTER_H
#define ICACHE_BUSTER_H

#include <vector>

class ICacheBuster {{
public:
  ICacheBuster(size_t num_methods);
  void RunNextMethod();
private:
  std::vector<void (*)()> methods_;
  size_t current_index_;
  size_t num_subset_methods_;
}};

#endif
"""

INIT_METHOD_DECL_TEMPALTE = (
    "extern void ICBInit_{SPLIT_NUM}" "(std::vector<void (*)()>& methods);"
)

SOURCE_TEMPLATE = """
#include <algorithm>
#include <cassert>
#include <chrono>
#include <numeric>
#include <random>
#include "ICacheBuster.h"

{INIT_METHOD_DECLS}

ICacheBuster::ICacheBuster(size_t num_methods)
    : methods_({NUM_METHODS}), current_index_(0),
      num_subset_methods_(num_methods) {{
  assert(num_methods <= {NUM_METHODS});
{INIT_METHOD_CALLS}
  // make a random permutation over data
  unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
  std::shuffle(methods_.begin(), methods_.end(), std::default_random_engine(seed));
}}

void ICacheBuster::RunNextMethod() {{
  methods_[current_index_]();
  current_index_ = (current_index_ + 1) % num_subset_methods_;
}}
"""

INIT_METHOD_CALL_TEMPLATE = "  ICBInit_{SPLIT_NUM}(methods_);"

METHOD_CODE_TEMPLATE = "void ICBMethod_{METHOD_NUM}() {{ }}"

INIT_METHOD_CODE_TEMPLATE = """
void ICBInit_{SPLIT_NUM}(std::vector<void (*)()>& methods) {{
{STORE_METHODS_CODE}
}}
"""

STORE_METHOD_CODE_TEMPLATE = "  methods[{METHOD_NUM}] = " "&ICBMethod_{METHOD_NUM};"


def grouper(n, iterable, fillvalue=None):
    args = [iter(iterable)] * n
    results = [
        [e for e in t if e is not None]
        for t in itertools.zip_longest(*args, fillvalue=fillvalue)
    ]
    if len(results) > 1 and len(results[-1]) != len(results[-2]):
        results[-2] += results[-1]
        del results[-1]
    return results


def main():
    # Parse arguments
    parser = argparse.ArgumentParser(description="Generate icache busting class")
    parser.add_argument(
        "--num_methods", type=int, help="Number of methods to generate", required=True
    )
    parser.add_argument(
        "--output_dir", help="Location to save generated code", required=True
    )
    parser.add_argument(
        "--num_splits",
        help="Number of ways to split files for fast compilation",
        type=int,
        default=1,
    )
    args = parser.parse_args()

    # Generate the files
    with open(args.output_dir + "/ICacheBuster.h", "w") as f:
        f.write(HEADER_TEMPLATE.format())

    splits = grouper(args.num_methods // args.num_splits, range(args.num_methods))
    for split_num in range(len(splits)):
        with open("%s/ICacheBuster.part%d.cc" % (args.output_dir, split_num), "w") as f:
            f.write("#include <vector>\n\n")
            methods_code = "\n".join(
                [METHOD_CODE_TEMPLATE.format(METHOD_NUM=i) for i in splits[split_num]]
            )
            f.write(methods_code)
            store_methods_code = "\n".join(
                [
                    STORE_METHOD_CODE_TEMPLATE.format(METHOD_NUM=i)
                    for i in splits[split_num]
                ]
            )
            f.write(
                INIT_METHOD_CODE_TEMPLATE.format(
                    STORE_METHODS_CODE=store_methods_code, SPLIT_NUM=split_num
                )
            )

    with open(args.output_dir + "/ICacheBuster.cc", "w") as f:
        init_methods_decl = "\n".join(
            [
                INIT_METHOD_DECL_TEMPALTE.format(SPLIT_NUM=i)
                for i in range(args.num_splits)
            ]
        )
        init_method_calls = "\n".join(
            [
                INIT_METHOD_CALL_TEMPLATE.format(SPLIT_NUM=i)
                for i in range(args.num_splits)
            ]
        )
        f.write(
            SOURCE_TEMPLATE.format(
                NUM_METHODS=args.num_methods,
                INIT_METHOD_DECLS=init_methods_decl,
                INIT_METHOD_CALLS=init_method_calls,
            )
        )


if __name__ == "__main__":
    main()

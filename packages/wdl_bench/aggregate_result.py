#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import glob
import json
import sys

import parse_line

sum_c = {}


for n in glob.glob("output_file_*"):
    with open(n) as f:
        if sys.argv[1] == "lzbench":
            parse_line.parse_line_lzbench(f, sum_c)
        else:
            parse_line.parse_line(f, sum_c)

out_file_name = "out_" + sys.argv[1] + ".json"
with open(out_file_name, "w") as f:
    json.dump(sum_c, f, indent=4, sort_keys=True)

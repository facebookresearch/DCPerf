#!/usr/bin/env python3
# Copyright (c) 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.
import logging
import re

from benchpress.lib.parser import Parser

logger = logging.getLogger(__name__)

re_array = r"array\_(\d+\w+)"
re_stride = r"stride\_(\d+\w+)"
re_thread = r"threads\_(\d+)"


class MultichasePointerParser(Parser):
    """
    input format:
    pointer_chase through an 256MB_array and stride_256B for 2.5 sec
    88.274
    pointer_chase through an 4MB_array w/ stride_64B
    31.010
    pointer chase through an 1GB_array stride_256B for 10 sec
    99.758
    pointer_chase through 256KB_array stride size 128B 2 threads
    Thread 0 accesses every 128th byte,
    thread 1 accesses every 128th byte offset by sizeof(void*)=8
    on 64bit architectures
    8.881
    """

    def parse(self, stdout, stderr, returncode):
        metrics = {}
        array_size = 0
        stride_size = 0
        thread_count = 0
        for line in stdout:
            array_sizes = re.findall(re_array, line)
            stride_sizes = re.findall(re_stride, line)
            thread_counts = re.findall(re_thread, line)
            if len(array_sizes) > 0:
                array_size = array_sizes[0]
            if len(thread_counts) > 0:
                thread_count = thread_counts[0]
            if len(stride_sizes) > 0:
                stride_size = stride_sizes[0]
                continue
            vals = line.split(" ")
            if len(vals) > 2:
                continue
            for v in vals:
                if len(v) == 0:
                    continue
                key = "pointer chase array " + array_size + " stride " + stride_size
                if thread_count != 0:
                    key = key + " thread count " + thread_count
                key = key + " latency (ns)"
                metrics[key] = float(v.strip())
        print("metrics", metrics)
        return metrics

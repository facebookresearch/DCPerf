#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

 MALLOC_CONF=narenas:20,dirty_decay_ms:5000 build/workloads/ranking/LeafNodeRank \
     --graph_scale=20 \
     --graph_subset=1572864 \
     --threads=36 \
     --cpu_threads=22 \
     --timekeeper_threads=2 \
     --io_threads=4 \
     --srv_threads=8 \
     --srv_io_threads=36 \
     --num_objects=2000 \
     --graph_max_iters=1 \
     --noaffinity \
     --min_icache_iterations=1600000

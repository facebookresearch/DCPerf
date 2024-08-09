#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

BPKGS_FFMPEG_ROOT="$(dirname "$(readlink -f "$0")")"
BENCHPRESS_ROOT="$(readlink -f "$BPKGS_FFMPEG_ROOT/../..")"
BENCHMARKS_DIR="${BENCHPRESS_ROOT}/benchmarks"
FFMPEG_DIR="${BENCHMARKS_DIR}/ffmpeg_video_workload"

rm -rf "$FFMPEG_DIR"

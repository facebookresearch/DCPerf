#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
set -Eeuo pipefail

MEM_MICRO_DIR="$(dirname "$(readlink -f "$0")")"

echo "Cleaning up mem_micro directory..."

cd "$MEM_MICRO_DIR" || exit 1

# Remove downloaded STREAM source
if [ -f stream.c ]; then
  echo "Removing stream.c..."
  rm -f stream.c
fi

# Remove compiled binaries
if [ -f stream-super-large-array ]; then
  echo "Removing stream-super-large-array binary..."
  rm -f stream-super-large-array
fi

# Remove any other STREAM binaries that might have been created
echo "Removing any other STREAM binaries..."
rm -f stream stream.exe a.out

# Remove log files
if [ -f stream_run.log ]; then
  echo "Removing stream_run.log..."
  rm -f stream_run.log
fi

# Remove any other log files
echo "Removing any other log files..."
rm -f ./*.log

echo "Cleanup complete!"

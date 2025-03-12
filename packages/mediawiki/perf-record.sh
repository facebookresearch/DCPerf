#!/bin/bash

sleep 30
if [ -f "perf.data" ]; then
  exit 0;
fi
if [ "${DCPERF_PERF_RECORD}" = "1" ]; then
  sudo nohup bash -xec "sleep 30; timeout -s INT 5 perf record -a -g;" > /tmp/mw-perf-record.log 2>&1 &
fi

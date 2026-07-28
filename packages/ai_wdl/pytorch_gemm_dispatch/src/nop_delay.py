#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""High-precision delay for GPU latency simulation.

nanosleep() has ~30-50 us minimum overhead due to kernel scheduling.
For simulating GPU compute latencies (e.g. 14 us for 1024^3 GEMM on GB200),
we need sub-microsecond precision.

Two modes:
  NopTimer  — NOP spin loop, requires calibration.  High instruction count.
  SpinTimer — clock_gettime spin loop, no calibration.  Minimal instruction
              pollution (~500 clock reads for 14 us vs millions of NOPs).
              Closer to real CUDA driver spin-wait behaviour.

Usage:
    timer = SpinTimer()       # preferred — low instruction overhead
    timer.delay_ns(14000)     # 14 us delay

    timer = NopTimer()        # legacy — high instruction overhead
    timer.delay_ns(14000)
"""

import _nop_delay_C as _C


class NopTimer:
    """High-precision delay timer using NOP spin loops.

    Calibrates at construction time by running NOPs for `calibration_ms`
    milliseconds and measuring throughput.

    Warning: injects millions of trivially-decoded NOP instructions that
    artificially lower cache MPKI and inflate Retiring%.  Prefer SpinTimer
    when collecting perf counters.
    """

    def __init__(self, calibration_ms: float = 100.0) -> None:
        self.nops_per_ns: float = _C.calibrate(calibration_ms)

    def delay_ns(self, nanos: float) -> None:
        """Busy-wait for approximately `nanos` nanoseconds."""
        if nanos > 0:
            _C.nop_delay_ns(nanos, self.nops_per_ns)

    def delay_s(self, seconds: float) -> None:
        """Busy-wait for approximately `seconds` seconds."""
        self.delay_ns(seconds * 1e9)


class SpinTimer:
    """High-precision delay by spin-polling clock_gettime(CLOCK_MONOTONIC).

    No calibration needed.  Executes very few instructions per iteration
    (~one VDSO call + compare + branch), so icache/dcache pollution is
    minimal.  This closely mimics the CUDA driver's spin-wait during
    cudaDeviceSynchronize.
    """

    def delay_ns(self, nanos: float) -> None:
        """Busy-wait for approximately `nanos` nanoseconds."""
        if nanos > 0:
            _C.spin_delay_ns(nanos)

    def delay_s(self, seconds: float) -> None:
        """Busy-wait for approximately `seconds` seconds."""
        self.delay_ns(seconds * 1e9)

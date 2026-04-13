#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Stage 1: GPU-less GEMM benchmark via TorchDispatchMode interception.

Measures the Python-level dispatch overhead of torch.mm by intercepting
aten.mm at the TorchDispatchMode level. Optionally simulates GPU latency
to mimic real execution timing.

Key mode: --no-sleep disables simulated delay to measure pure host-side
dispatch overhead, which is the primary metric for BTB analysis.

Runs on any machine — no GPU or CUDA drivers needed.
"""

import argparse
import sys
import time

import torch
from gpu_timing_model import GPUTimingConfig, variant_from_str
from nop_delay import NopTimer
from stage1_dispatch_mode import GpulessMmMode
from torch.profiler import profile, ProfilerActivity


_DTYPE_MAP = {
    "float32": torch.float32,
    "float16": torch.float16,
    "bfloat16": torch.bfloat16,
}


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="GPU-less GEMM benchmark (Stage 1: TorchDispatchMode)"
    )
    p.add_argument("-m", "--msize", type=int, default=1024, help="M dimension")
    p.add_argument("-n", "--nsize", type=int, default=1024, help="N dimension")
    p.add_argument("-k", "--ksize", type=int, default=1024, help="K dimension")
    p.add_argument(
        "-t",
        "--dtype",
        type=str,
        default="bfloat16",
        choices=list(_DTYPE_MAP.keys()),
        help="Data type (default: bfloat16)",
    )
    p.add_argument("--steps", type=int, default=100, help="Number of timed iterations")
    p.add_argument(
        "--warmups", type=int, default=10, help="Number of warmup iterations"
    )
    p.add_argument(
        "--gpu-model",
        type=str,
        default="gb200",
        help="GPU variant to simulate: gb200, gb300, h100 (default: gb200)",
    )
    p.add_argument(
        "--efficiency",
        type=float,
        default=0.5,
        help="GPU efficiency factor 0.0-1.0 (default: 0.5)",
    )
    p.add_argument(
        "--no-sleep",
        action="store_true",
        help="Disable simulated GPU delay — measures pure host dispatch overhead",
    )
    p.add_argument(
        "--trace",
        type=str,
        default=None,
        metavar="PATH",
        help="Export PyTorch profiler trace to file (Chrome/Perfetto .json or .json.gz)",
    )
    return p.parse_args()


def run_benchmark(args: argparse.Namespace) -> None:
    m, n, k = args.msize, args.nsize, args.ksize
    dtype = _DTYPE_MAP[args.dtype]

    variant = variant_from_str(args.gpu_model)
    config = GPUTimingConfig(variant=variant, efficiency=args.efficiency)

    nop_timer = None
    if not args.no_sleep:
        print("Calibrating NOP timer...", flush=True)
        nop_timer = NopTimer(calibration_ms=200)
        print(f"  NOP rate: {nop_timer.nops_per_ns:.3f} nops/ns")

    mode = GpulessMmMode(config=config, sleep=not args.no_sleep, nop_timer=nop_timer)

    # Create input tensors on CPU
    a = torch.randn(m, k, dtype=dtype, device="cpu")
    b = torch.randn(k, n, dtype=dtype, device="cpu")

    flops_per_call = 2.0 * m * n * k

    # Warmup phase
    with mode:
        for _ in range(args.warmups):
            torch.mm(a, b)
    mode.stats.reset()

    # Measured phase
    with mode:
        prof_ctx = profile(activities=[ProfilerActivity.CPU]) if args.trace else None
        if prof_ctx is not None:
            prof_ctx.__enter__()
        t0 = time.perf_counter()
        for _ in range(args.steps):
            torch.mm(a, b)
        t1 = time.perf_counter()
        if prof_ctx is not None:
            prof_ctx.__exit__(None, None, None)

    wall_time = t1 - t0
    calls = mode.stats.call_count
    simulated_gpu_time = mode.stats.total_simulated_time_s

    if calls == 0:
        print("ERROR: No GEMM calls intercepted", file=sys.stderr)
        sys.exit(1)

    wall_per_call = wall_time / calls
    simulated_per_call = simulated_gpu_time / calls

    if args.no_sleep:
        host_overhead_per_call = wall_per_call
    else:
        host_overhead_per_call = wall_per_call - simulated_per_call

    simulated_tfs = flops_per_call * calls / wall_time / 1e12 if wall_time > 0 else 0.0

    # Report
    print(f"{'=' * 60}")
    print("Stage 1: GPU-less GEMM Benchmark (TorchDispatchMode)")
    print(f"{'=' * 60}")
    print(f"  Matrix:       ({m} x {k}) @ ({k} x {n})")
    print(f"  Dtype:        {args.dtype}")
    print(f"  GPU model:    {args.gpu_model} (efficiency={args.efficiency})")
    print(f"  Sleep:        {'disabled (--no-sleep)' if args.no_sleep else 'enabled'}")
    print(f"  Steps:        {args.steps}  (warmups: {args.warmups})")
    print(f"{'=' * 60}")
    print(f"  Total wall time:        {wall_time * 1e3:12.3f} ms")
    print(f"  Wall time / call:       {wall_per_call * 1e6:12.3f} us")
    if not args.no_sleep:
        print(f"  Simulated GPU / call:   {simulated_per_call * 1e6:12.3f} us")
    print(f"  Host overhead / call:   {host_overhead_per_call * 1e6:12.3f} us")
    print(f"  Simulated TF/s:         {simulated_tfs:12.6f}")
    print(f"  Intercepted calls:      {calls}")
    for op_name, count in sorted(mode.stats.per_op_counts.items()):
        print(f"    {op_name}: {count}")
    print(f"{'=' * 60}")

    if args.trace and prof_ctx is not None:
        prof_ctx.export_chrome_trace(args.trace)
        print(f"  Trace exported to: {args.trace}")


def main() -> None:
    args = parse_args()
    run_benchmark(args)


if __name__ == "__main__":
    main()

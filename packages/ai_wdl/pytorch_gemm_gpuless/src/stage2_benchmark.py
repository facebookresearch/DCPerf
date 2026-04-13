#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Stage 2: GPU-less GEMM benchmark via mock_cuda.

Measures the full host-side overhead of torch.mm including C++ dispatch,
cuBLAS wrapper code, and CUDA driver API call overhead — everything except
actual GPU kernel execution.

Patches the function table in libcuda.so.1 so all CUDA driver calls
(cuLaunchKernel, cuMemAlloc, etc.) return success instantly without GPU work.

Requires: CUDA drivers installed (libcuda.so.1 must be present), but no GPU.
"""

import argparse
import time

import torch
from gpu_timing_model import compute_mm_latency, GPUTimingConfig, variant_from_str
from mock_cuda_guard import mock_cuda_guard
from nop_delay import NopTimer, SpinTimer
from torch.profiler import profile, ProfilerActivity


_DTYPE_MAP = {
    "float32": torch.float32,
    "float16": torch.float16,
    "bfloat16": torch.bfloat16,
}


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="GPU-less GEMM benchmark (Stage 2: mock_cuda)"
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
        "--delay-mode",
        type=str,
        default="nop",
        choices=["nop", "spin"],
        help="Delay method: nop = NOP spin loop (default), "
        "spin = clock_gettime spin (minimal instruction pollution)",
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
    flops_per_call = 2.0 * m * n * k

    variant = variant_from_str(args.gpu_model)
    config = GPUTimingConfig(variant=variant, efficiency=args.efficiency)
    simulated_latency = compute_mm_latency(m, n, k, config)
    simulated_latency_ns = simulated_latency * 1e9
    do_sleep = not args.no_sleep

    delay_timer = None
    if do_sleep:
        if args.delay_mode == "spin":
            delay_timer = SpinTimer()
            print("Using spin delay (clock_gettime polling)", flush=True)
        else:
            print("Calibrating NOP timer...", flush=True)
            nop_timer = NopTimer(calibration_ms=200)
            print(f"  NOP rate: {nop_timer.nops_per_ns:.3f} nops/ns")
            delay_timer = nop_timer

    with mock_cuda_guard():
        # Create CUDA tensors (backed by fake memory under mock)
        a = torch.randn(m, k, dtype=dtype, device="cuda:0")
        b = torch.randn(k, n, dtype=dtype, device="cuda:0")

        # Warmup
        for _ in range(args.warmups):
            torch.mm(a, b)
            if do_sleep and delay_timer is not None:
                delay_timer.delay_ns(simulated_latency_ns)
        torch.cuda.synchronize()  # no-op under mock

        # Measured phase
        activities = [ProfilerActivity.CPU, ProfilerActivity.CUDA]
        prof_ctx = profile(activities=activities) if args.trace else None
        if prof_ctx is not None:
            prof_ctx.__enter__()
        t0 = time.perf_counter()
        for _ in range(args.steps):
            torch.mm(a, b)
            if do_sleep and delay_timer is not None:
                delay_timer.delay_ns(simulated_latency_ns)
        torch.cuda.synchronize()  # no-op under mock
        t1 = time.perf_counter()
        if prof_ctx is not None:
            prof_ctx.__exit__(None, None, None)

    wall_time = t1 - t0
    wall_per_call = wall_time / args.steps
    total_simulated_gpu_time = simulated_latency * args.steps

    if args.no_sleep:
        host_overhead_per_call = wall_per_call
    else:
        host_overhead_per_call = wall_per_call - simulated_latency

    tfs = flops_per_call * args.steps / wall_time / 1e12 if wall_time > 0 else 0.0

    # Report
    print(f"{'=' * 60}")
    print("Stage 2: GPU-less GEMM Benchmark (mock_cuda)")
    print(f"{'=' * 60}")
    print(f"  Matrix:       ({m} x {k}) @ ({k} x {n})")
    print(f"  Dtype:        {args.dtype}")
    print(f"  GPU model:    {args.gpu_model} (efficiency={args.efficiency})")
    if args.no_sleep:
        sleep_str = "disabled (--no-sleep)"
    else:
        sleep_str = f"enabled ({args.delay_mode})"
    print(f"  Sleep:        {sleep_str}")
    print(f"  Steps:        {args.steps}  (warmups: {args.warmups})")
    print(f"{'=' * 60}")
    print(f"  Total wall time:        {wall_time * 1e3:12.3f} ms")
    print(f"  Wall time / call:       {wall_per_call * 1e6:12.3f} us")
    if not args.no_sleep:
        print(f"  Simulated GPU / call:   {simulated_latency * 1e6:12.3f} us")
    print(f"  Host overhead / call:   {host_overhead_per_call * 1e6:12.3f} us")
    print(f"  Simulated TF/s:         {tfs:12.6f}")
    print(f"{'=' * 60}")

    if args.trace and prof_ctx is not None:
        prof_ctx.export_chrome_trace(args.trace)
        print(f"  Trace exported to: {args.trace}")


def main() -> None:
    args = parse_args()
    run_benchmark(args)


if __name__ == "__main__":
    main()

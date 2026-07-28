#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Stage 1: GPU-less GEMM benchmark via TorchDispatchMode interception.

Measures the Python-level dispatch overhead of one GEMM-like op by
intercepting `aten::mm`, `aten::addmm`, `aten::bmm`, or `aten::linear`
at the TorchDispatchMode level. Optionally simulates GPU latency to mimic
real execution timing.

Key mode: --no-sleep disables simulated delay to measure pure host-side
dispatch overhead, which is the primary metric for BTB analysis.

Runs on any machine — no GPU or CUDA drivers needed.
"""

import argparse
import sys
import time

import torch
from gemm_ops import (
    add_standalone_gemm_shape_args,
    describe_gemm_spec,
    make_standalone_gemm_spec_from_args,
    run_gemm_op,
)
from gpu_timing_model import GPUTimingConfig, variant_from_str
from nop_delay import NopTimer, SpinTimer
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
    add_standalone_gemm_shape_args(p)
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
    dtype = _DTYPE_MAP[args.dtype]
    try:
        spec = make_standalone_gemm_spec_from_args(args)
    except ValueError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(2)

    variant = variant_from_str(args.gpu_model)
    config = GPUTimingConfig(variant=variant, efficiency=args.efficiency)

    delay_timer = None
    if not args.no_sleep:
        if args.delay_mode == "spin":
            delay_timer = SpinTimer()
            print("Using spin delay (clock_gettime polling)", flush=True)
        else:
            print("Calibrating NOP timer...", flush=True)
            nop_timer = NopTimer(calibration_ms=200)
            print(f"  NOP rate: {nop_timer.nops_per_ns:.3f} nops/ns")
            delay_timer = nop_timer

    mode = GpulessMmMode(
        config=config,
        sleep=not args.no_sleep,
        delay_timer=delay_timer,
    )

    # Create input tensors on CPU
    tensors = [
        torch.empty(spec.a_shape, dtype=dtype, device="cpu"),
        torch.empty(spec.b_shape, dtype=dtype, device="cpu"),
    ]
    if spec.bias_shape is not None:
        tensors.append(torch.empty(spec.bias_shape, dtype=dtype, device="cpu"))
    tensors_tuple = tuple(tensors)

    flops_per_call = spec.flops_per_call

    # Warmup phase
    with mode:
        for _ in range(args.warmups):
            run_gemm_op(spec.op, tensors_tuple)
    mode.stats.reset()

    # Measured phase
    with mode:
        prof_ctx = profile(activities=[ProfilerActivity.CPU]) if args.trace else None
        if prof_ctx is not None:
            prof_ctx.__enter__()
        t0 = time.perf_counter()
        for _ in range(args.steps):
            run_gemm_op(spec.op, tensors_tuple)
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
    print(f"  Op:           {spec.op}")
    print(f"  Shapes:       {describe_gemm_spec(spec)}")
    print(f"  Effective GEMM: M={spec.effective_m} N={spec.n} K={spec.k}")
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

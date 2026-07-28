#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Stage 2 GPU-less benchmark driven by a YAML GEMM workload spec.

Reads a YAML config exported by `gputrace-analysis/hot_functions.py
--export-gemm-yaml ...` (a list of `(op, input_types, shapes,
occurrences_per_iter, weight)` entries) and runs each GEMM with the configured
shape/dtype under `mock_cuda_guard()`. Reports total wall time, total FLOPs,
and the resulting simulated TF/s for the workload.

Supported ops: aten::mm, aten::addmm, aten::bmm, aten::linear.

Example:
    ./benchmarks/ai_wdl/pytorch_gemm_dispatch/run.sh stage2_with_specs \\
        gemm_specs/model_c.yaml --iterations 3 --delay-mode spin \\
        --gpu-model gb200 --efficiency 0.5
"""

import argparse
import sys
import time
from dataclasses import dataclass, field
from typing import List, Optional, Tuple

import torch
import yaml
from gemm_ops import gemm_spec_from_shapes, run_gemm_op
from gpu_timing_model import GPUTimingConfig, GPUVariant, variant_from_str


# Map the short dtype names emitted by gputrace-analysis (and the raw c10
# names found in some traces) to torch dtypes.
_DTYPE_MAP = {
    "bf16": torch.bfloat16,
    "fp16": torch.float16,
    "fp32": torch.float32,
    "fp64": torch.float64,
    "c10::BFloat16": torch.bfloat16,
    "c10::Half": torch.float16,
    "float": torch.float32,
    "double": torch.float64,
}


@dataclass
class GemmSig:
    """One GEMM signature ready to execute."""

    op: str
    dtype: torch.dtype
    a_shape: Tuple[int, ...]
    b_shape: Tuple[int, ...]
    bias_shape: Optional[Tuple[int, ...]]  # addmm/linear only
    flops_per_call: float
    calls_per_iter: int  # rounded from occurrences_per_iter
    occurrences_per_iter: float  # original (un-rounded) value
    weight: float
    raw_input_types: Tuple[str, ...] = field(default_factory=tuple)

    @property
    def dtype_str(self) -> str:
        return str(self.dtype).replace("torch.", "")


@dataclass
class SigStats:
    """Per-signature accumulated runtime."""

    total_wall_s: float = 0.0
    total_calls: int = 0
    total_flops: float = 0.0


# --------------------------------------------------------------------------- #
# YAML parsing
# --------------------------------------------------------------------------- #


def _parse_shape(s: object, where: str) -> Tuple[int, ...]:
    if not isinstance(s, list):
        raise ValueError(f"{where}: expected list of ints, got {s!r}")
    return tuple(int(d) for d in s)


def _flops_for_op(
    op: str, shapes: List[Tuple[int, ...]]
) -> Tuple[float, Tuple[int, ...], Tuple[int, ...], Optional[Tuple[int, ...]]]:
    spec = gemm_spec_from_shapes(op, shapes)
    return spec.flops_per_call, spec.a_shape, spec.b_shape, spec.bias_shape


def parse_yaml(path: str) -> Tuple[List[GemmSig], List[str]]:
    """Parse a GEMM YAML config into a list of executable signatures.

    Returns (sigs, warnings). `warnings` lists entries that were skipped
    (unsupported op, bad shape, unknown dtype, etc.).
    """
    with open(path) as f:
        data = yaml.safe_load(f)
    if not isinstance(data, dict) or "gemm_ops" not in data:
        raise ValueError(f"{path}: missing 'gemm_ops' key")

    sigs: List[GemmSig] = []
    warnings: List[str] = []
    for idx, entry in enumerate(data["gemm_ops"]):
        try:
            op = entry["op"]
            input_types = tuple(str(t) for t in entry["input_types"])
            raw_shapes = entry["shapes"]
            shapes = [_parse_shape(s, f"entry #{idx}") for s in raw_shapes]
            occurrences = float(entry.get("occurrences_per_iter", 1.0))
            weight = float(entry.get("weight", 0.0))

            dtype = _DTYPE_MAP.get(input_types[0]) if input_types else None
            if dtype is None:
                warnings.append(
                    f"entry #{idx} ({op}): unsupported dtype {input_types[0]!r} — skipped"
                )
                continue

            spec = gemm_spec_from_shapes(op, shapes)
            calls_per_iter = max(1, int(round(occurrences)))

            sigs.append(
                GemmSig(
                    op=spec.op,
                    dtype=dtype,
                    a_shape=spec.a_shape,
                    b_shape=spec.b_shape,
                    bias_shape=spec.bias_shape,
                    flops_per_call=spec.flops_per_call,
                    calls_per_iter=calls_per_iter,
                    occurrences_per_iter=occurrences,
                    weight=weight,
                    raw_input_types=input_types,
                )
            )
        except (KeyError, ValueError) as e:
            warnings.append(f"entry #{idx}: {e} — skipped")
    return sigs, warnings


# --------------------------------------------------------------------------- #
# Execution
# --------------------------------------------------------------------------- #


def _allocate(sig: GemmSig, device: str = "cuda:0") -> Tuple[torch.Tensor, ...]:
    """Allocate the input tensors for one signature."""
    a = torch.empty(*sig.a_shape, dtype=sig.dtype, device=device)
    b = torch.empty(*sig.b_shape, dtype=sig.dtype, device=device)
    if sig.bias_shape is not None:
        bias = torch.empty(*sig.bias_shape, dtype=sig.dtype, device=device)
        return a, b, bias
    return a, b


def _run_op(sig: GemmSig, tensors: Tuple[torch.Tensor, ...]) -> None:
    run_gemm_op(sig.op, tensors)


def _latency_seconds(flops: float, peak_tflops: float, efficiency: float) -> float:
    throughput = peak_tflops * efficiency * 1e12
    return flops / throughput if throughput > 0 else 0.0


# --------------------------------------------------------------------------- #
# Reporting
# --------------------------------------------------------------------------- #


def _shape_str(sig: GemmSig) -> str:
    parts = [
        f"[{','.join(str(d) for d in sig.a_shape)}]",
        f"[{','.join(str(d) for d in sig.b_shape)}]",
    ]
    if sig.bias_shape is not None:
        parts.insert(0, f"[{','.join(str(d) for d in sig.bias_shape)}]")
    return "x".join(parts)


def _print_breakdown(sigs: List[GemmSig], stats: List[SigStats], top_n: int) -> None:
    indices = sorted(
        range(len(sigs)),
        key=lambda i: stats[i].total_wall_s,
        reverse=True,
    )[:top_n]
    print()
    print(f"Top {len(indices)} signatures by total wall time:")
    print(
        f"  {'Op':<12} {'Dtype':<6} {'Shapes':<46} {'Calls':>8} "
        f"{'Wall(ms)':>10} {'TF/s':>8}"
    )
    print("  " + "-" * 96)
    for i in indices:
        sig = sigs[i]
        st = stats[i]
        tfs = st.total_flops / st.total_wall_s / 1e12 if st.total_wall_s > 0 else 0.0
        shapes = _shape_str(sig)[:46]
        print(
            f"  {sig.op:<12} {sig.dtype_str:<6} {shapes:<46} {st.total_calls:>8} "
            f"{st.total_wall_s * 1e3:>10.2f} {tfs:>8.2f}"
        )


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Replay a GEMM YAML workload through the GPU-less harness.",
    )
    p.add_argument("yaml_path", help="Path to a GEMM YAML config")
    p.add_argument(
        "--iterations",
        type=int,
        default=3,
        help="Number of full-workload iterations to replay (default: 3)",
    )
    p.add_argument(
        "--warmup-iterations",
        type=int,
        default=1,
        help="Number of warmup iterations (default: 1)",
    )
    p.add_argument(
        "--top-n",
        type=int,
        default=0,
        help=(
            "Only run the top-N signatures by weight (default: 0 = all). "
            "Use for fast smoke tests."
        ),
    )
    p.add_argument(
        "--min-weight",
        type=float,
        default=0.0,
        help="Drop signatures with weight < this threshold (default: 0)",
    )
    p.add_argument(
        "--breakdown",
        type=int,
        default=10,
        metavar="N",
        help="Print per-signature breakdown for the top N by wall time (default: 10)",
    )
    p.add_argument(
        "--gpu-model",
        type=str,
        default="gb200",
        choices=[v.value for v in GPUVariant],
        help=(
            "GPU variant for simulated GPU latency. Picks a BF16 peak TFLOPS "
            "from gpu_timing_model. Override with --peak-tflops to simulate "
            "any GPU not in this list. (default: gb200)"
        ),
    )
    p.add_argument(
        "--peak-tflops",
        type=float,
        default=None,
        metavar="TFLOPS",
        help=(
            "Override the GPU variant's BF16 dense peak TFLOPS. Use to "
            "simulate a custom GPU or override a preset variant."
        ),
    )
    p.add_argument(
        "--efficiency",
        type=float,
        default=0.5,
        metavar="FRAC",
        help="GPU efficiency factor in (0, 1] (default: 0.5)",
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
        help="Delay method: nop = NOP spin loop, spin = clock_gettime polling",
    )
    return p.parse_args()


def _filter_sigs(sigs: List[GemmSig], args: argparse.Namespace) -> List[GemmSig]:
    if args.min_weight > 0:
        sigs = [s for s in sigs if s.weight >= args.min_weight]
    sigs.sort(key=lambda s: s.weight, reverse=True)
    if args.top_n > 0:
        sigs = sigs[: args.top_n]
    return sigs


def _make_delay_timer(args: argparse.Namespace):
    if args.no_sleep:
        return None
    from nop_delay import NopTimer, SpinTimer

    if args.delay_mode == "spin":
        print("Using spin delay (clock_gettime polling)", flush=True)
        return SpinTimer()
    print("Calibrating NOP timer...", flush=True)
    nop_timer = NopTimer(calibration_ms=200)
    print(f"  NOP rate: {nop_timer.nops_per_ns:.3f} nops/ns", flush=True)
    return nop_timer


def _run_workload(
    sigs: List[GemmSig],
    tensors: List[Tuple[torch.Tensor, ...]],
    latency_ns: List[float],
    stats: List[SigStats],
    iterations: int,
    delay_timer,
) -> float:
    """Replay each signature `calls_per_iter` times, `iterations` times.

    Each call is wall-timed individually so we can produce a per-signature
    breakdown. Returns total wall time in seconds.
    """
    do_sleep = delay_timer is not None
    total_t0 = time.perf_counter()
    for _ in range(iterations):
        for i, sig in enumerate(sigs):
            t = tensors[i]
            ns = latency_ns[i]
            st = stats[i]
            for _ in range(sig.calls_per_iter):
                s0 = time.perf_counter()
                _run_op(sig, t)
                if do_sleep:
                    delay_timer.delay_ns(ns)
                s1 = time.perf_counter()
                st.total_wall_s += s1 - s0
                st.total_calls += 1
                st.total_flops += sig.flops_per_call
    return time.perf_counter() - total_t0


def main() -> None:
    from mock_cuda_guard import mock_cuda_guard

    args = parse_args()

    sigs, warnings = parse_yaml(args.yaml_path)
    for w in warnings:
        print(f"warning: {w}", file=sys.stderr)
    if not sigs:
        print("error: no usable signatures parsed from YAML", file=sys.stderr)
        sys.exit(1)

    sigs = _filter_sigs(sigs, args)
    if not sigs:
        print("error: filter dropped all signatures", file=sys.stderr)
        sys.exit(1)

    if args.peak_tflops is not None:
        peak_tflops = args.peak_tflops
        peak_source = "user override"
    else:
        peak_tflops = GPUTimingConfig(
            variant=variant_from_str(args.gpu_model), efficiency=args.efficiency
        ).peak_tflops
        peak_source = f"{args.gpu_model} preset"
    delay_timer = _make_delay_timer(args)
    latency_ns = [
        _latency_seconds(s.flops_per_call, peak_tflops, args.efficiency) * 1e9
        for s in sigs
    ]

    total_calls_per_iter = sum(s.calls_per_iter for s in sigs)
    flops_per_iter = sum(s.flops_per_call * s.calls_per_iter for s in sigs)

    print(f"{'=' * 70}")
    print(f"YAML: {args.yaml_path}")
    print(
        f"Signatures: {len(sigs)}  "
        f"Calls/iter: {total_calls_per_iter:,}  "
        f"FLOPs/iter: {flops_per_iter / 1e12:.3f} T"
    )
    print(
        f"GPU: peak={peak_tflops:.1f} TF/s ({peak_source})  "
        f"eff={args.efficiency}  "
        f"Sleep: {'off' if args.no_sleep else args.delay_mode}  "
        f"Iterations: {args.iterations} (warmup: {args.warmup_iterations})"
    )
    print(f"{'=' * 70}", flush=True)

    stats = [SigStats() for _ in sigs]
    do_sleep = not args.no_sleep

    with mock_cuda_guard():
        # Pre-allocate all tensors once. Under mock_cuda these are fake
        # allocations so the cost is small even for thousands of signatures.
        tensors = [_allocate(s) for s in sigs]

        # Warmup runs are not timed.
        warmup_stats = [SigStats() for _ in sigs]
        if args.warmup_iterations > 0:
            _run_workload(
                sigs,
                tensors,
                latency_ns,
                warmup_stats,
                args.warmup_iterations,
                delay_timer,
            )

        total_wall = _run_workload(
            sigs, tensors, latency_ns, stats, args.iterations, delay_timer
        )

    total_calls = sum(s.total_calls for s in stats)
    total_flops = sum(s.total_flops for s in stats)
    sim_tfs = total_flops / total_wall / 1e12 if total_wall > 0 else 0.0
    sim_gpu_s = (
        sum(latency_ns[i] * stats[i].total_calls for i in range(len(sigs))) / 1e9
    )
    host_wall = total_wall - sim_gpu_s if do_sleep else total_wall

    print()
    print(f"  Total wall time:        {total_wall * 1e3:12.2f} ms")
    print(f"  Total calls:            {total_calls:12,}")
    print(f"  Total FLOPs:            {total_flops / 1e12:12.3f} T")
    if do_sleep:
        print(f"  Simulated GPU time:     {sim_gpu_s * 1e3:12.2f} ms")
        print(f"  Host overhead:          {host_wall * 1e3:12.2f} ms")
    print(f"  Simulated TF/s:         {sim_tfs:12.3f}")
    print(f"  Per-iter wall time:     {total_wall / args.iterations * 1e3:12.2f} ms")
    print(f"{'=' * 70}")

    if args.breakdown > 0:
        _print_breakdown(sigs, stats, args.breakdown)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""TorchDispatchMode-based interception of GEMM ops for GPU-less benchmarking.

Intercepts `aten::mm`, `aten::addmm`, `aten::bmm`, and `aten::linear` at the
Python dispatch level, optionally simulating GPU latency. Returns
correctly-shaped CPU tensors without performing actual computation.

Pattern follows _FlopCounterMode in caffe2/torch/utils/flop_counter.py.
"""

import time
from dataclasses import dataclass, field
from typing import Optional, Protocol

import torch
from gemm_ops import gemm_spec_from_shapes
from gpu_timing_model import compute_mm_latency, GPUTimingConfig
from torch.utils._python_dispatch import TorchDispatchMode


class DelayTimer(Protocol):
    def delay_s(self, seconds: float) -> None: ...


@dataclass
class GpulessMmStats:
    """Accumulated statistics from intercepted GEMM operations."""

    call_count: int = 0
    total_simulated_time_s: float = 0.0
    per_op_counts: dict[str, int] = field(default_factory=dict)

    def reset(self) -> None:
        self.call_count = 0
        self.total_simulated_time_s = 0.0
        self.per_op_counts.clear()

    def record(self, op_name: str, simulated_latency_s: float) -> None:
        self.call_count += 1
        self.total_simulated_time_s += simulated_latency_s
        self.per_op_counts[op_name] = self.per_op_counts.get(op_name, 0) + 1


_FUNC_TO_OP = {
    torch.ops.aten.mm.default: "aten::mm",
    torch.ops.aten.addmm.default: "aten::addmm",
    torch.ops.aten.bmm.default: "aten::bmm",
    torch.ops.aten.linear.default: "aten::linear",
}

# Ops we intercept
_INTERCEPTED_OPS = frozenset(_FUNC_TO_OP)


class GpulessMmMode(TorchDispatchMode):
    """Intercepts GEMM ops, returns zero tensors, optionally simulates GPU latency.

    Usage:
        config = GPUTimingConfig(variant=GPUVariant.GB200, efficiency=0.5)
        mode = GpulessMmMode(config=config, sleep=True)
        with mode:
            c = torch.mm(a, b)  # intercepted, returns zeros
        print(mode.stats)

    Args:
        config: GPU timing configuration for latency simulation.
        sleep: If True, delay for the simulated latency duration.
            Set to False to measure pure host-side dispatch overhead.
        delay_timer: If provided, use a calibrated NOP timer or a spin timer
            instead of time.sleep. Required for sub-microsecond precision.
    """

    def __init__(
        self,
        config: GPUTimingConfig | None = None,
        sleep: bool = True,
        delay_timer: Optional[DelayTimer] = None,
    ) -> None:
        super().__init__()
        self.config = config or GPUTimingConfig()
        self.sleep = sleep
        self.delay_timer = delay_timer
        self.stats = GpulessMmStats()

    def __torch_dispatch__(self, func, types, args=(), kwargs=None):
        kwargs = kwargs if kwargs else {}

        if func not in _INTERCEPTED_OPS:
            return func(*args, **kwargs)

        op_name = _FUNC_TO_OP[func]
        if func is torch.ops.aten.mm.default:
            tensor_shapes = [tuple(args[0].shape), tuple(args[1].shape)]
            dtype = args[0].dtype
        elif func is torch.ops.aten.addmm.default:
            tensor_shapes = [
                tuple(args[0].shape),
                tuple(args[1].shape),
                tuple(args[2].shape),
            ]
            dtype = args[1].dtype
        elif func is torch.ops.aten.bmm.default:
            tensor_shapes = [tuple(args[0].shape), tuple(args[1].shape)]
            dtype = args[0].dtype
        else:
            tensor_shapes = [tuple(args[0].shape), tuple(args[1].shape)]
            if len(args) > 2 and args[2] is not None:
                tensor_shapes.append(tuple(args[2].shape))
            dtype = args[0].dtype

        spec = gemm_spec_from_shapes(op_name, tensor_shapes)

        # Compute simulated latency
        latency = compute_mm_latency(spec.effective_m, spec.n, spec.k, self.config)
        self.stats.record(op_name.replace("::", "."), latency)

        if self.sleep:
            if self.delay_timer is not None:
                self.delay_timer.delay_s(latency)
            else:
                time.sleep(latency)

        return torch.zeros(spec.output_shape, dtype=dtype, device="cpu")

#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""TorchDispatchMode-based interception of GEMM ops for GPU-less benchmarking.

Intercepts aten.mm, aten.addmm, and aten.bmm at the Python dispatch level,
optionally simulating GPU latency. Returns correctly-shaped CPU tensors
without performing actual computation.

Pattern follows _FlopCounterMode in caffe2/torch/utils/flop_counter.py.
"""

import time
from dataclasses import dataclass, field
from typing import Optional

import torch
from gpu_timing_model import compute_mm_latency, GPUTimingConfig
from nop_delay import NopTimer
from torch.utils._python_dispatch import TorchDispatchMode


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


# Ops we intercept
_INTERCEPTED_OPS = frozenset(
    {
        torch.ops.aten.mm.default,
        torch.ops.aten.addmm.default,
        torch.ops.aten.bmm.default,
    }
)


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
        nop_timer: If provided, use NOP spin loop for delay instead of
            time.sleep. Required for sub-microsecond precision.
    """

    def __init__(
        self,
        config: GPUTimingConfig | None = None,
        sleep: bool = True,
        nop_timer: Optional[NopTimer] = None,
    ) -> None:
        super().__init__()
        self.config = config or GPUTimingConfig()
        self.sleep = sleep
        self.nop_timer = nop_timer
        self.stats = GpulessMmStats()

    def __torch_dispatch__(self, func, types, args=(), kwargs=None):
        kwargs = kwargs if kwargs else {}

        if func not in _INTERCEPTED_OPS:
            return func(*args, **kwargs)

        # Compute output shape and latency based on which op we intercepted
        if func is torch.ops.aten.mm.default:
            a, b = args[0], args[1]
            m, k = a.shape
            _, n = b.shape
            out_shape = (m, n)
            dtype = a.dtype
            op_name = "aten.mm"

        elif func is torch.ops.aten.addmm.default:
            # addmm(bias, input, weight) -> bias + input @ weight
            a, b = args[1], args[2]
            m, k = a.shape
            _, n = b.shape
            out_shape = (m, n)
            dtype = a.dtype
            op_name = "aten.addmm"

        elif func is torch.ops.aten.bmm.default:
            a, b = args[0], args[1]
            batch, m, k = a.shape
            _, _, n = b.shape
            out_shape = (batch, m, n)
            dtype = a.dtype
            op_name = "aten.bmm"

        else:
            # Should not reach here given _INTERCEPTED_OPS check above
            return func(*args, **kwargs)

        # Compute simulated latency
        latency = compute_mm_latency(m, n, k, self.config)
        self.stats.record(op_name, latency)

        if self.sleep:
            if self.nop_timer is not None:
                self.nop_timer.delay_s(latency)
            else:
                time.sleep(latency)

        return torch.zeros(out_shape, dtype=dtype, device="cpu")

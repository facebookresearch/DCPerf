#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""GPU latency simulation model for GPU-less GEMM benchmarking.

Computes the expected GPU execution time for matrix multiply operations
based on theoretical peak throughput and an efficiency factor.
"""

from dataclasses import dataclass
from enum import Enum


class GPUVariant(Enum):
    """Supported GPU variants with their BF16 dense peak TFLOPS."""

    GB200 = "gb200"
    GB300 = "gb300"
    H100 = "h100"


# Peak BF16 dense TFLOPS for each GPU variant
_PEAK_TFLOPS: dict[GPUVariant, float] = {
    GPUVariant.GB200: 2250.0,
    GPUVariant.GB300: 2250.0,  # Same Blackwell die as GB200
    GPUVariant.H100: 1979.0,
}

# Minimum latency floor in seconds
_MIN_LATENCY_S: float = 0


@dataclass
class GPUTimingConfig:
    """Configuration for GPU latency simulation.

    Args:
        variant: GPU variant to simulate.
        efficiency: Fraction of peak throughput achieved (0.0-1.0).
            Typical values: 0.3-0.7 depending on problem size.
    """

    variant: GPUVariant = GPUVariant.GB200
    efficiency: float = 0.5

    def __post_init__(self) -> None:
        if not 0.0 < self.efficiency <= 1.0:
            raise ValueError(f"efficiency must be in (0.0, 1.0], got {self.efficiency}")

    @property
    def peak_tflops(self) -> float:
        return _PEAK_TFLOPS[self.variant]


def compute_mm_latency(m: int, n: int, k: int, config: GPUTimingConfig) -> float:
    """Compute simulated GPU latency for a matrix multiply (M x K) @ (K x N).

    Formula: latency = 2*M*N*K / (peak_tflops * efficiency * 1e12)
    The result is floored at 5 microseconds to account for kernel launch overhead.

    Args:
        m: Number of rows of the output matrix.
        n: Number of columns of the output matrix.
        k: Shared dimension (inner dimension).
        config: GPU timing configuration.

    Returns:
        Simulated latency in seconds.
    """
    flops = 2.0 * m * n * k
    throughput = config.peak_tflops * config.efficiency * 1e12
    latency = flops / throughput
    return max(latency, _MIN_LATENCY_S)


def variant_from_str(name: str) -> GPUVariant:
    """Parse a GPU variant name string (case-insensitive).

    Args:
        name: One of "gb200", "gb300", "h100".

    Returns:
        The corresponding GPUVariant enum member.

    Raises:
        ValueError: If the name is not recognized.
    """
    try:
        return GPUVariant(name.lower())
    except ValueError:
        valid = ", ".join(v.value for v in GPUVariant)
        raise ValueError(f"Unknown GPU variant '{name}'. Valid options: {valid}")

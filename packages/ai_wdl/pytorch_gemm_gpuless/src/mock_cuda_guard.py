#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Python wrapper for our custom mock_cuda C++ extension.

Provides mock_cuda_guard() context manager that patches libcuda.so.1's
function table so CUDA driver calls return success instantly without
GPU work. Compatible with NVIDIA driver 570.x+.

See docs/driver_binary_analysis.md for details on the binary patching.
"""

import ctypes
from contextlib import contextmanager
from typing import Generator, Optional

import _mock_cuda_C as _C
import torch

# Patch at import time — installs our mock functions into
# libcuda.so.1's function table. The mocks are disabled by default
# (they forward to the real functions). Call enable_mock_cuda() to
# activate the no-op behavior.
_C.patch_mock_cuda()

_mock_cuda_stream: Optional[torch.cuda.Stream] = None


def _has_real_gpu() -> bool:
    """Check for GPU by querying the driver directly via ctypes.

    This avoids torch.cuda.is_available() which can return False when
    the driver version is below what the PyTorch build expects.
    """
    try:
        libcuda = ctypes.CDLL("libcuda.so.1")
        count = ctypes.c_int(0)
        if libcuda.cuInit(0) != 0:
            return False
        libcuda.cuDeviceGetCount(ctypes.byref(count))
        return count.value > 0
    except OSError:
        return False


def _init_cuda_with_mock() -> None:
    """Initialize PyTorch CUDA with mock enabled to bypass driver version checks.

    PyTorch checks cuDriverGetVersion >= CUDA runtime version. When the
    driver is older (e.g. 570.x = CUDA 12.8 vs PyTorch CUDA 13.0), this
    check fails. We temporarily enable mock so cuDriverGetVersion returns
    a high value, then disable mock for normal operation.

    Do NOT call torch.cuda.is_available() first — it triggers the same
    version check and caches the failure.
    """
    _C.enable_mock_cuda()
    try:
        torch.cuda.init()
    finally:
        _C.disable_mock_cuda()


def _get_mock_cuda_stream() -> torch.cuda.Stream:
    global _mock_cuda_stream
    if _mock_cuda_stream is None:
        _mock_cuda_stream = torch.cuda.Stream()
    return _mock_cuda_stream


@contextmanager
def mock_cuda_guard() -> Generator[None, None, None]:
    """Context manager that enables CUDA mocking.

    All CUDA driver calls within the context return success instantly
    without performing GPU work. Memory allocations return fake pointers
    in the address space above 1UL << 48.

    Requires a real GPU — NVIDIA confirmed the CUDA driver prevents
    API interception on GPU-less machines by design.
    """
    if not _has_real_gpu():
        raise RuntimeError(
            "Stage 2 requires a real GPU. NVIDIA's CUDA driver prevents "
            "API interception on GPU-less machines by design. "
            "Use Stage 1 for CPU-only machines."
        )

    # Initialize CUDA (handles driver version mismatch via mock).
    _init_cuda_with_mock()

    # Pre-initialize cuBLAS before enabling mock — cublasCreate
    # allocates real GPU workspace memory that must not be faked.
    stream = _get_mock_cuda_stream()
    with torch.cuda.stream(stream):
        torch.cuda.current_blas_handle()

    try:
        with torch.cuda.stream(stream):
            _C.enable_mock_cuda()
            yield
    finally:
        _C.disable_mock_cuda()


def mock_cuda() -> None:
    """Enable CUDA mocking (thread-local)."""
    _C.enable_mock_cuda()


def unmock_cuda() -> None:
    """Disable CUDA mocking (thread-local)."""
    _C.disable_mock_cuda()

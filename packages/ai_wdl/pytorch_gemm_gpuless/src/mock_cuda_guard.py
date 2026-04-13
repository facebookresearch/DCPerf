#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Python wrapper for our custom mock_cuda C++ extension.

Provides mock_cuda_guard() context manager that patches libcuda.so.1's
function table so CUDA driver calls return success instantly without
GPU work. Compatible with NVIDIA driver 580.x+.

See docs/driver_binary_analysis.md for details on the binary patching.
"""

from contextlib import contextmanager
from typing import Generator, Optional
from unittest.mock import patch

import _mock_cuda_C as _C
import torch

# Patch at import time — installs our mock functions into
# libcuda.so.1's function table. The mocks are disabled by default
# (they forward to the real functions). Call enable_mock_cuda() to
# activate the no-op behavior.
_C.patch_mock_cuda()

_mock_cuda_stream: Optional[torch.cuda.Stream] = None
_has_real_gpu: Optional[bool] = None


def _detect_real_gpu() -> bool:
    """Check if a real GPU is available (cached)."""
    global _has_real_gpu
    if _has_real_gpu is None:
        try:
            _has_real_gpu = torch.cuda.device_count() > 0
        except Exception:
            _has_real_gpu = False
    return _has_real_gpu


def _get_mock_cuda_stream() -> torch.cuda.Stream:
    global _mock_cuda_stream
    if _mock_cuda_stream is None:
        _mock_cuda_stream = torch.cuda.Stream()
    return _mock_cuda_stream


def _fake_lazy_init() -> None:
    """No-op replacement for torch.cuda._lazy_init on GPU-less machines."""
    pass


@contextmanager
def mock_cuda_guard() -> Generator[None, None, None]:
    """Context manager that enables CUDA mocking.

    All CUDA driver calls within the context return success instantly
    without performing GPU work. Memory allocations return fake pointers
    in the address space above 1UL << 48.

    On machines with a real GPU, uses a dedicated CUDA stream.
    On GPU-less machines, monkey-patches PyTorch's CUDA initialization
    to bypass device count checks.
    """
    has_gpu = _detect_real_gpu()

    if has_gpu:
        # Normal path: use a dedicated stream on real GPU machines
        try:
            with torch.cuda.stream(_get_mock_cuda_stream()):
                _C.enable_mock_cuda()
                yield
        finally:
            _C.disable_mock_cuda()
    else:
        # GPU-less path: patch PyTorch's CUDA init to skip device checks
        patches = [
            patch("torch.cuda._lazy_init", _fake_lazy_init),
            patch("torch.cuda.device_count", return_value=1),
            patch("torch.cuda.is_available", return_value=True),
        ]
        try:
            for p in patches:
                p.start()
            _C.enable_mock_cuda()
            yield
        finally:
            _C.disable_mock_cuda()
            for p in reversed(patches):
                p.stop()


def mock_cuda() -> None:
    """Enable CUDA mocking (thread-local)."""
    _C.enable_mock_cuda()


def unmock_cuda() -> None:
    """Disable CUDA mocking (thread-local)."""
    _C.disable_mock_cuda()

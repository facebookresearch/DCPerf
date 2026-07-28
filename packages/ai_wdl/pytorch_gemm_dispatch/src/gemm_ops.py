#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

"""Shared GEMM shape, FLOP, and dispatch helpers."""

import argparse
from dataclasses import dataclass
from math import prod
from typing import Optional, Sequence

import torch


SUPPORTED_GEMM_OP_ALIASES = (
    "mm",
    "aten::mm",
    "addmm",
    "aten::addmm",
    "bmm",
    "aten::bmm",
    "linear",
    "aten::linear",
)

_OP_ALIASES = {
    "mm": "aten::mm",
    "aten::mm": "aten::mm",
    "addmm": "aten::addmm",
    "aten::addmm": "aten::addmm",
    "bmm": "aten::bmm",
    "aten::bmm": "aten::bmm",
    "linear": "aten::linear",
    "aten::linear": "aten::linear",
}


@dataclass(frozen=True)
class GemmOpSpec:
    """Canonical description of one GEMM-like operation."""

    op: str
    a_shape: tuple[int, ...]
    b_shape: tuple[int, ...]
    bias_shape: Optional[tuple[int, ...]]
    output_shape: tuple[int, ...]
    effective_m: int
    n: int
    k: int

    @property
    def flops_per_call(self) -> float:
        return 2.0 * self.effective_m * self.n * self.k


def canonicalize_gemm_op(op: str) -> str:
    normalized = op.strip().lower()
    canonical = _OP_ALIASES.get(normalized)
    if canonical is None:
        valid = ", ".join(SUPPORTED_GEMM_OP_ALIASES)
        raise ValueError(f"Unsupported GEMM op {op!r}. Valid options: {valid}")
    return canonical


def _shape_tuple(shape: Sequence[int]) -> tuple[int, ...]:
    dims = tuple(int(d) for d in shape)
    if any(d <= 0 for d in dims):
        raise ValueError(f"shape dimensions must be positive, got {dims}")
    return dims


def _is_broadcastable(shape: tuple[int, ...], target: tuple[int, ...]) -> bool:
    if len(shape) > len(target):
        return False
    for dim, target_dim in zip(reversed(shape), reversed(target)):
        if dim not in (1, target_dim):
            return False
    return True


def parse_shape_arg(value: str) -> tuple[int, ...] | None:
    stripped = value.strip()
    if stripped.lower() in {"auto", "none", "null", "default"}:
        return None
    if stripped.lower() in {"scalar", "()"}:
        return ()

    normalized = stripped.replace("x", ",").replace("X", ",")
    parts = [part.strip() for part in normalized.split(",")]
    if not parts or any(part == "" for part in parts):
        raise argparse.ArgumentTypeError(
            f"Invalid shape {value!r}; use comma- or x-separated positive ints"
        )

    try:
        return _shape_tuple(int(part) for part in parts)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(str(exc)) from exc


def add_standalone_gemm_shape_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--op",
        type=str,
        default="mm",
        choices=SUPPORTED_GEMM_OP_ALIASES,
        help="GEMM op to benchmark (default: mm)",
    )
    parser.add_argument("-m", "--msize", type=int, default=1024, help="M dimension")
    parser.add_argument("-n", "--nsize", type=int, default=1024, help="N dimension")
    parser.add_argument("-k", "--ksize", type=int, default=1024, help="K dimension")
    parser.add_argument(
        "--batch-size",
        "--bmm-batch-size",
        dest="batch_size",
        type=int,
        default=1,
        help="Batch size for `bmm`, or shorthand for one leading dimension on "
        "`linear` when `--linear-prefix-shape` is not set",
    )
    parser.add_argument(
        "--addmm-bias-shape",
        type=parse_shape_arg,
        default=None,
        help="Shape for `addmm`'s input/bias tensor. Must broadcast to the "
        "MxN output. Examples: `256`, `1,256`, `128x256`, `scalar`, `auto`",
    )
    parser.add_argument(
        "--linear-prefix-shape",
        type=parse_shape_arg,
        default=None,
        help="Leading dims before K for `linear`. Examples: `128`, `8,128`, "
        "`4x8x128`, `scalar`, `auto`. Overrides `-m` and `--batch-size` for "
        "`linear`",
    )
    parser.add_argument(
        "--linear-no-bias",
        action="store_true",
        help="Disable the optional bias vector for `linear`",
    )


def make_standalone_gemm_spec_from_args(args: argparse.Namespace) -> "GemmOpSpec":
    return make_standalone_gemm_spec(
        args.op,
        args.msize,
        args.nsize,
        args.ksize,
        args.batch_size,
        addmm_bias_shape=args.addmm_bias_shape,
        linear_prefix_shape=args.linear_prefix_shape,
        linear_has_bias=not args.linear_no_bias,
    )


def gemm_spec_from_shapes(op: str, shapes: Sequence[Sequence[int]]) -> GemmOpSpec:
    """Infer the effective GEMM dimensions for one op signature."""

    canonical = canonicalize_gemm_op(op)
    normalized_shapes = [_shape_tuple(shape) for shape in shapes]

    if canonical == "aten::mm":
        if len(normalized_shapes) < 2:
            raise ValueError(
                f"aten::mm requires >=2 shapes, got {len(normalized_shapes)}"
            )
        a_shape, b_shape = normalized_shapes[0], normalized_shapes[1]
        if len(a_shape) != 2 or len(b_shape) != 2 or a_shape[1] != b_shape[0]:
            raise ValueError(f"aten::mm shape mismatch: {a_shape} @ {b_shape}")
        m, k = a_shape
        _, n = b_shape
        return GemmOpSpec(
            op=canonical,
            a_shape=a_shape,
            b_shape=b_shape,
            bias_shape=None,
            output_shape=(m, n),
            effective_m=m,
            n=n,
            k=k,
        )

    if canonical == "aten::addmm":
        if len(normalized_shapes) < 3:
            raise ValueError(
                f"aten::addmm requires >=3 shapes, got {len(normalized_shapes)}"
            )
        bias_shape, a_shape, b_shape = (
            normalized_shapes[0],
            normalized_shapes[1],
            normalized_shapes[2],
        )
        if len(a_shape) != 2 or len(b_shape) != 2 or a_shape[1] != b_shape[0]:
            raise ValueError(f"aten::addmm shape mismatch: {a_shape} @ {b_shape}")
        m, k = a_shape
        _, n = b_shape
        output_shape = (m, n)
        if not _is_broadcastable(bias_shape, output_shape):
            raise ValueError(
                f"aten::addmm bias shape {bias_shape} is not broadcastable to "
                f"{output_shape}"
            )
        return GemmOpSpec(
            op=canonical,
            a_shape=a_shape,
            b_shape=b_shape,
            bias_shape=bias_shape,
            output_shape=output_shape,
            effective_m=m,
            n=n,
            k=k,
        )

    if canonical == "aten::bmm":
        if len(normalized_shapes) < 2:
            raise ValueError(
                f"aten::bmm requires >=2 shapes, got {len(normalized_shapes)}"
            )
        a_shape, b_shape = normalized_shapes[0], normalized_shapes[1]
        if (
            len(a_shape) != 3
            or len(b_shape) != 3
            or a_shape[0] != b_shape[0]
            or a_shape[2] != b_shape[1]
        ):
            raise ValueError(f"aten::bmm shape mismatch: {a_shape} @ {b_shape}")
        batch, m, k = a_shape
        _, _, n = b_shape
        return GemmOpSpec(
            op=canonical,
            a_shape=a_shape,
            b_shape=b_shape,
            bias_shape=None,
            output_shape=(batch, m, n),
            effective_m=batch * m,
            n=n,
            k=k,
        )

    if len(normalized_shapes) < 2:
        raise ValueError(
            f"aten::linear requires >=2 shapes, got {len(normalized_shapes)}"
        )
    a_shape, b_shape = normalized_shapes[0], normalized_shapes[1]
    if len(a_shape) < 1 or len(b_shape) != 2 or a_shape[-1] != b_shape[1]:
        raise ValueError(
            f"aten::linear shape mismatch: input={a_shape} weight={b_shape}"
        )
    bias_shape = normalized_shapes[2] if len(normalized_shapes) > 2 else None
    n, k = b_shape
    if bias_shape is not None and bias_shape != (n,):
        raise ValueError(f"aten::linear bias must have shape {(n,)}, got {bias_shape}")
    return GemmOpSpec(
        op=canonical,
        a_shape=a_shape,
        b_shape=b_shape,
        bias_shape=bias_shape,
        output_shape=a_shape[:-1] + (n,),
        effective_m=prod(a_shape[:-1]) if len(a_shape) > 1 else 1,
        n=n,
        k=k,
    )


def make_standalone_gemm_spec(
    op: str,
    m: int,
    n: int,
    k: int,
    batch_size: int = 1,
    *,
    addmm_bias_shape: Sequence[int] | None = None,
    linear_prefix_shape: Sequence[int] | None = None,
    linear_has_bias: bool = True,
) -> GemmOpSpec:
    """Construct a standalone benchmark problem from CLI dimensions."""

    if m <= 0 or n <= 0 or k <= 0:
        raise ValueError(f"m, n, k must be positive, got m={m}, n={n}, k={k}")
    if batch_size <= 0:
        raise ValueError(f"batch_size must be positive, got {batch_size}")

    canonical = canonicalize_gemm_op(op)
    normalized_addmm_bias_shape = (
        _shape_tuple(addmm_bias_shape) if addmm_bias_shape is not None else None
    )
    normalized_linear_prefix_shape = (
        _shape_tuple(linear_prefix_shape) if linear_prefix_shape is not None else None
    )

    if canonical != "aten::addmm" and normalized_addmm_bias_shape is not None:
        raise ValueError("--addmm-bias-shape only applies to `addmm`")
    if canonical != "aten::linear" and normalized_linear_prefix_shape is not None:
        raise ValueError("--linear-prefix-shape only applies to `linear`")
    if canonical != "aten::linear" and not linear_has_bias:
        raise ValueError("--linear-no-bias only applies to `linear`")
    if canonical in {"aten::mm", "aten::addmm"} and batch_size != 1:
        raise ValueError("--batch-size only applies to `bmm` and `linear`")

    if canonical == "aten::mm":
        shapes = [(m, k), (k, n)]
    elif canonical == "aten::addmm":
        shapes = [
            normalized_addmm_bias_shape
            if normalized_addmm_bias_shape is not None
            else (n,),
            (m, k),
            (k, n),
        ]
    elif canonical == "aten::bmm":
        shapes = [(batch_size, m, k), (batch_size, k, n)]
    else:
        if normalized_linear_prefix_shape is not None:
            if batch_size != 1:
                raise ValueError(
                    "`--batch-size` cannot be combined with `--linear-prefix-shape`"
                )
            input_shape = normalized_linear_prefix_shape + (k,)
        else:
            input_shape = (batch_size, m, k) if batch_size > 1 else (m, k)
        shapes = [input_shape, (n, k)]
        if linear_has_bias:
            shapes.append((n,))

    return gemm_spec_from_shapes(canonical, shapes)


def describe_gemm_spec(spec: GemmOpSpec) -> str:
    if spec.op == "aten::mm":
        return f"A={_format_shape(spec.a_shape)} @ B={_format_shape(spec.b_shape)}"
    if spec.op == "aten::addmm":
        return (
            f"bias={_format_shape(spec.bias_shape)} + "
            f"A={_format_shape(spec.a_shape)} @ B={_format_shape(spec.b_shape)}"
        )
    if spec.op == "aten::bmm":
        return f"A={_format_shape(spec.a_shape)} @ B={_format_shape(spec.b_shape)}"
    bias = (
        f", bias={_format_shape(spec.bias_shape)}"
        if spec.bias_shape is not None
        else ""
    )
    return (
        f"input={_format_shape(spec.a_shape)}, "
        f"weight={_format_shape(spec.b_shape)}{bias}"
    )


def run_gemm_op(op: str, tensors: tuple[torch.Tensor, ...]) -> None:
    canonical = canonicalize_gemm_op(op)
    if canonical == "aten::mm":
        torch.mm(tensors[0], tensors[1])
    elif canonical == "aten::addmm":
        torch.addmm(tensors[2], tensors[0], tensors[1])
    elif canonical == "aten::bmm":
        torch.bmm(tensors[0], tensors[1])
    elif canonical == "aten::linear":
        bias = tensors[2] if len(tensors) > 2 else None
        torch.ops.aten.linear.default(tensors[0], tensors[1], bias)
    else:
        raise ValueError(f"Unsupported op: {op}")


def _format_shape(shape: Optional[tuple[int, ...]]) -> str:
    if shape is None:
        return "None"
    return "(" + " x ".join(str(dim) for dim in shape) + ")"

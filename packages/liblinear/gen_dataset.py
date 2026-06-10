#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Generate an arbitrary-size synthetic binary-classification dataset in the
sparse LIBSVM format that liblinear's ``train``/``predict`` CLIs consume.

A random ground-truth hyperplane labels each sample by the sign of the linear
score, producing linearly-separable data up to a configurable amount of label
noise. Output rows use 1-based ascending feature indices and always contain at
least one nonzero feature (liblinear rejects empty rows). Stdlib only.
"""

from __future__ import annotations

import argparse
import random


def generate_weight_vector(
    n_features: int, rng: random.Random
) -> tuple[list[float], float]:
    weights = [rng.uniform(-1.0, 1.0) for _ in range(n_features)]
    bias = rng.uniform(-1.0, 1.0)
    return weights, bias


def generate_sample(n_features: int, density: float, rng: random.Random) -> list[float]:
    features = [0.0] * n_features
    for i in range(n_features):
        if rng.random() < density:
            features[i] = rng.uniform(-1.0, 1.0)
    # liblinear rejects empty rows: force at least one nonzero feature.
    if not any(features):
        idx = rng.randrange(n_features)
        features[idx] = rng.uniform(-1.0, 1.0) or 1.0
    return features


def compute_label(
    features: list[float],
    weights: list[float],
    bias: float,
    noise: float,
    rng: random.Random,
) -> int:
    score = bias + sum(f * w for f, w in zip(features, weights))
    label = 1 if score >= 0.0 else -1
    if noise > 0.0 and rng.random() < noise:
        label = -label
    return label


def format_libsvm_line(label: int, features: list[float]) -> str:
    parts = [str(label)]
    for i, value in enumerate(features):
        if value != 0.0:
            parts.append(f"{i + 1}:{value:.6f}")
    return " ".join(parts)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--n-samples", type=int, default=2000)
    parser.add_argument("--n-features", type=int, default=50)
    parser.add_argument("--train-fraction", type=float, default=0.8)
    parser.add_argument("--density", type=float, default=0.7)
    parser.add_argument("--noise", type=float, default=0.05)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--train-out", default="train.libsvm")
    parser.add_argument("--test-out", default="test.libsvm")
    args = parser.parse_args()

    rng = random.Random(args.seed)
    weights, bias = generate_weight_vector(args.n_features, rng)

    n_train = int(args.n_samples * args.train_fraction)
    with open(args.train_out, "w") as train_f, open(args.test_out, "w") as test_f:
        for i in range(args.n_samples):
            features = generate_sample(args.n_features, args.density, rng)
            label = compute_label(features, weights, bias, args.noise, rng)
            line = format_libsvm_line(label, features)
            out = train_f if i < n_train else test_f
            out.write(line + "\n")

    print(
        f"wrote {n_train} train rows to {args.train_out} and "
        f"{args.n_samples - n_train} test rows to {args.test_out}"
    )


if __name__ == "__main__":
    main()

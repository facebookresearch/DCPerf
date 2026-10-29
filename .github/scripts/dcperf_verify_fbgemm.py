#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import math
import os
import signal
import subprocess
import sys
from pathlib import Path

BENCHPRESS_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(BENCHPRESS_ROOT))

from benchpress.plugins.parsers.fbgemm import FbgemmParser


def smoke_cases():
    cases = [
        ("fp16", ["FP16Benchmark", "--repit=1", "--no-flush", "--no-mkl"]),
        ("spmdm8", ["EmbeddingSpMDM8BitBenchmark", "--smoke"]),
        (
            "embedding_a_single",
            [
                "tbe_inference_benchmark",
                "nbit-cpu",
                "--num-embeddings=1000",
                "--bag-size=2",
                "--embedding-dim=96",
                "--batch-size=8",
                "--num-tables=2",
                "--weights-precision=int4",
                "--output-dtype=fp32",
                "--copies=2",
                "--iters=10",
            ],
        ),
    ]
    for name, precision, dim in [
        ("embedding_a_spec", "int4", 96),
        ("embedding_b_spec_int4", "int4", 64),
        ("embedding_b_spec_int8", "int8", 64),
    ]:
        cases.append(
            (
                name,
                [
                    "tbe_inference_benchmark",
                    "nbit-device-with-spec",
                    "--num-embeddings-list=1000,1000",
                    "--bag-size-list=1,2",
                    f"--embedding-dim-list={dim},{dim}",
                    "--batch-size=8",
                    f"--weights-precision={precision}",
                    "--output-dtype=fp32",
                    "--cpu-copies=2",
                    "--iters=10",
                    "--runs-of-iters=2",
                    "--warmup-runs=1",
                    "--use-cpu",
                ],
            )
        )
    return cases


def positive(value):
    return isinstance(value, (int, float)) and math.isfinite(value) and value > 0


def validate_metrics(name, metrics):
    if name == "fp16":
        values = [v for k, v in metrics.items() if k.startswith("CPU_A:")]
        valid = values and all(positive(v.get("Gflops")) for v in values)
    elif name == "spmdm8":
        values = metrics.get("embedding_spmdm8bit", [])
        valid = values and all(positive(v.get("b/w_GBps")) for v in values)
    else:
        valid = positive(metrics.get("bandwidth"))
    if not valid:
        raise RuntimeError(
            f"{name}: missing, nonfinite, or nonpositive metrics: {metrics}"
        )


def run_case(root, log_dir, name, args, timeout=600):
    env = os.environ.copy()
    env.update(OMP_NUM_THREADS="1", MKL_NUM_THREADS="1", OPENBLAS_NUM_THREADS="1")
    stdout_path = log_dir / f"{name}.stdout.log"
    stderr_path = log_dir / f"{name}.stderr.log"
    print(f"RUN: {name}; logs: {stdout_path}, {stderr_path}", flush=True)
    failure = None
    with stdout_path.open("w") as stdout, stderr_path.open("w") as stderr:
        process = subprocess.Popen(
            ["bash", str(root / "run.sh"), *args],
            env=env,
            stdout=stdout,
            stderr=stderr,
            start_new_session=True,
        )
        try:
            returncode = process.wait(timeout=timeout)
            if returncode:
                failure = f"exited {returncode}"
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait()
            failure = f"timed out after {timeout}s"
    stdout_lines = stdout_path.read_text().splitlines()
    stderr_lines = stderr_path.read_text().splitlines()
    if failure:
        raise RuntimeError(
            f"{name}: {failure}\n" + "\n".join((stdout_lines + stderr_lines)[-40:])
        )
    metrics = FbgemmParser().parse(stdout_lines, stderr_lines, returncode)
    validate_metrics(name, metrics)
    print(f"PASS: {name}", flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--benchmark-root",
        type=Path,
        default=BENCHPRESS_ROOT / "benchmarks/ai_wdl/fbgemm",
    )
    parser.add_argument(
        "--log-dir", type=Path, default=BENCHPRESS_ROOT / "ci_logs/fbgemm"
    )
    options = parser.parse_args()
    for binary in [
        "run.sh",
        "tbe_inference_benchmark",
        "FP16Benchmark",
        "EmbeddingSpMDM8BitBenchmark",
    ]:
        path = options.benchmark_root / binary
        if not path.is_file() or not os.access(path, os.X_OK):
            raise RuntimeError(f"Missing installed executable: {path}")
    options.log_dir.mkdir(parents=True, exist_ok=True)
    for name, args in smoke_cases():
        run_case(options.benchmark_root, options.log_dir, name, args)


if __name__ == "__main__":
    main()

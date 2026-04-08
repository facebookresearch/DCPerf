#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""
Reusable pre-flight checks for any benchmark.

Can be called from any benchmark's runner script (especially bash-based
runners that cannot call diagnosis_utils.py functions directly).
Diagnosis merging is handled at the framework level in run.py.

Usage:
    python3 preflight_checks.py --benchmark mediawiki --benchpress-root /path \
        [--auto-fix-ulimit] [--min-fds 100000]
"""

import argparse
import sys

from diagnosis_utils import (
    check_file_descriptor_limit,
    check_ipv6_hostname,
    check_selinux,
    DiagnosisRecorder,
)


# Default minimum file descriptors needed for a reliable run.
DEFAULT_MIN_FDS = 100_000


def run_checks(args):
    """Run pre-flight checks before the benchmark starts."""
    DiagnosisRecorder.get_instance(root_dir=args.benchpress_root)

    all_ok = True
    all_ok = (
        check_selinux(benchmark=args.benchmark, root_dir=args.benchpress_root)
        and all_ok
    )
    # check_ipv6_hostname returns True when IPv6 is detected (not necessarily
    # broken). It records a diagnosis failure internally only when IPv6 is
    # detected AND broken, so we don't use its return value for all_ok.
    check_ipv6_hostname(
        "localhost", benchmark=args.benchmark, root_dir=args.benchpress_root
    )
    all_ok = (
        check_file_descriptor_limit(
            benchmark=args.benchmark,
            required_fds=args.min_fds,
            auto_fix=args.auto_fix_ulimit,
            root_dir=args.benchpress_root,
        )
        and all_ok
    )

    if all_ok:
        print("\nAll pre-flight checks passed.")
    else:
        print(
            "\nSome pre-flight checks failed (see above). "
            "Continuing anyway so diagnostics appear in the output.",
            file=sys.stderr,
        )


def main():
    parser = argparse.ArgumentParser(
        description="Reusable pre-flight checks for benchpress benchmarks"
    )
    parser.add_argument(
        "--benchmark",
        default="unknown",
        help="Name of the benchmark (e.g., mediawiki, tao_bench)",
    )
    parser.add_argument(
        "--benchpress-root", default=".", help="Path to benchpress root directory"
    )
    parser.add_argument(
        "--min-fds",
        type=int,
        default=DEFAULT_MIN_FDS,
        help=f"Minimum file descriptor soft limit required (default: {DEFAULT_MIN_FDS})",
    )
    parser.add_argument(
        "--auto-fix-ulimit",
        action="store_true",
        help="Automatically raise file descriptor soft limit if too low",
    )
    args = parser.parse_args()

    run_checks(args)

    sys.exit(0)


if __name__ == "__main__":
    main()

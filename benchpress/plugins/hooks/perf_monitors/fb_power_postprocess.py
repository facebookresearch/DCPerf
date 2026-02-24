#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""Standalone post-processing script for fb-power.csv files.

Usage:
    python fb_power_postprocess.py \\
        --input benchmark_metrics_<uuid>/fb-power.csv \\
        --config fb_power/configs/t1_bgm.json \\
        --output benchmark_metrics_<uuid>/fb-power-summary.csv

    # Auto-detect platform:
    python fb_power_postprocess.py \\
        --input benchmark_metrics_<uuid>/fb-power.csv \\
        --auto-detect \\
        --output benchmark_metrics_<uuid>/fb-power-summary.csv
"""

import argparse
import importlib
import logging
import os
import sys

# Allow this script to be run standalone from the perf_monitors directory
# or as part of the benchpress package
try:
    from benchpress.plugins.hooks.perf_monitors.fb_power.post_process import (
        PowerPostProcessor,
    )
except ImportError:
    _mod = importlib.import_module("fb_power.post_process")
    PowerPostProcessor = _mod.PowerPostProcessor  # type: ignore[attr-defined]

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] %(name)s: %(message)s",
)
logger = logging.getLogger(__name__)


def main():
    parser = argparse.ArgumentParser(
        description="Post-process raw BMC power sensor data into decomposed power components"
    )
    parser.add_argument(
        "--input",
        required=True,
        help="Path to raw fb-power.csv",
    )
    parser.add_argument(
        "--config",
        help="Path to platform JSON config file",
    )
    parser.add_argument(
        "--auto-detect",
        action="store_true",
        help="Auto-detect platform and select config",
    )
    parser.add_argument(
        "--output",
        help="Output path for summary CSV (default: fb-power-summary.csv in same dir)",
    )
    parser.add_argument(
        "--list-configs",
        action="store_true",
        help="List available platform configs and exit",
    )
    args = parser.parse_args()

    if args.list_configs:
        configs = PowerPostProcessor.list_available_configs()
        print("Available platform configs:")
        for f in configs:
            print(f"  {f}")
        return

    config_path = args.config
    if args.auto_detect:
        import socket

        try:
            from benchpress.plugins.hooks.perf_monitors.fb_power.platform_detect import (
                detect_platform,
            )
        except ImportError:
            _mod = importlib.import_module("fb_power.platform_detect")
            detect_platform = _mod.detect_platform

        hostname = socket.gethostname()
        platform = detect_platform(hostname)
        config_path = PowerPostProcessor.get_config_path(platform)
        logger.info(f"Auto-detected platform: {platform}")

    if not config_path or not os.path.exists(config_path):
        print(f"Error: Config file not found: {config_path}", file=sys.stderr)
        print("Use --list-configs to see available configs", file=sys.stderr)
        sys.exit(1)

    output_path = args.output
    if not output_path:
        base = os.path.dirname(args.input)
        output_path = os.path.join(base, "fb-power-summary.csv")

    logger.info(f"Input: {args.input}")
    logger.info(f"Config: {config_path}")
    logger.info(f"Output: {output_path}")

    processor = PowerPostProcessor(config_path)
    result_df = processor.process(args.input)
    processor.write_summary(result_df, output_path)

    print(f"\nSummary written to: {output_path}")
    print(f"Columns: {', '.join(result_df.columns.tolist())}")
    print(f"Rows: {len(result_df)}")


if __name__ == "__main__":
    main()

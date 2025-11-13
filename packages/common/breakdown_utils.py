# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""
Breakdown utilities for benchmarks.

This module provides utilities for tracking benchmark runtime breakdown
by logging timestamps for different phases (preprocessing, main benchmark,
postprocessing) to a CSV file.
"""

import csv
from datetime import datetime
from pathlib import Path


# Constants matching the original bash script
BREAKDOWN_FILE_NAME: str = "breakdown.csv"
MAIN_OPERATION_NAME: str = "main_benchmark"
PREPROCESSING_OPERATION_NAME: str = "preprocessing"
POSTPROCESSING_OPERATION_NAME: str = "postprocessing"


def create_breakdown_csv(folder_path: str) -> None:
    """
    Create a CSV file for runtime breakdown tracking.

    Args:
        folder_path: Path to the folder where the CSV file will be created
    """
    folder = Path(folder_path)
    folder.mkdir(parents=True, exist_ok=True)

    csv_file = folder / BREAKDOWN_FILE_NAME
    with open(csv_file, "w", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(
            [
                "operation_name",
                "PID",
                "timestamp_type",
                "timestamp",
                "sub_operation_name",
            ]
        )


def _log_breakdown_entry(
    folder_path: str,
    operation_name: str,
    pid: int,
    timestamp_type: str,
    sub_operation_name: str = "",
) -> None:
    """
    Log an entry to the breakdown CSV file.

    Args:
        folder_path: Path to the folder containing the CSV file
        operation_name: Name of the operation (e.g., 'main_benchmark', 'preprocessing')
        pid: Process ID
        timestamp_type: Type of timestamp ('start' or 'end')
        sub_operation_name: Optional sub-operation name
    """
    csv_file = Path(folder_path) / BREAKDOWN_FILE_NAME

    # Get timestamp in format: YYYY-MM-DD HH:MM:SS.mmm (milliseconds)
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]

    with open(csv_file, "a", newline="") as f:
        writer = csv.writer(f)
        writer.writerow(
            [operation_name, pid, timestamp_type, timestamp, sub_operation_name]
        )


def log_preprocessing_start(folder_path: str, pid: int) -> None:
    """
    Log the start of the preprocessing operation.

    Args:
        folder_path: Path to the folder containing the CSV file
        pid: Process ID
    """
    _log_breakdown_entry(folder_path, PREPROCESSING_OPERATION_NAME, pid, "start")


def log_preprocessing_end(folder_path: str, pid: int) -> None:
    """
    Log the end of the preprocessing operation.

    Args:
        folder_path: Path to the folder containing the CSV file
        pid: Process ID
    """
    _log_breakdown_entry(folder_path, PREPROCESSING_OPERATION_NAME, pid, "end")


def log_preprocessing_warmup_start(folder_path: str, pid: int) -> None:
    """
    Log the start of the preprocessing warmup sub-operation.

    Args:
        folder_path: Path to the folder containing the CSV file
        pid: Process ID
    """
    _log_breakdown_entry(
        folder_path, PREPROCESSING_OPERATION_NAME, pid, "start", "warmup"
    )


def log_preprocessing_warmup_end(folder_path: str, pid: int) -> None:
    """
    Log the end of the preprocessing warmup sub-operation.

    Args:
        folder_path: Path to the folder containing the CSV file
        pid: Process ID
    """
    _log_breakdown_entry(
        folder_path, PREPROCESSING_OPERATION_NAME, pid, "end", "warmup"
    )


def log_main_benchmark_start(folder_path: str, pid: int) -> None:
    """
    Log the start of the main benchmark operation.

    Args:
        folder_path: Path to the folder containing the CSV file
        pid: Process ID
    """
    _log_breakdown_entry(folder_path, MAIN_OPERATION_NAME, pid, "start")


def log_main_benchmark_end(folder_path: str, pid: int) -> None:
    """
    Log the end of the main benchmark operation.

    Args:
        folder_path: Path to the folder containing the CSV file
        pid: Process ID
    """
    _log_breakdown_entry(folder_path, MAIN_OPERATION_NAME, pid, "end")


def log_postprocessing_start(folder_path: str, pid: int) -> None:
    """
    Log the start of the postprocessing operation.

    Args:
        folder_path: Path to the folder containing the CSV file
        pid: Process ID
    """
    _log_breakdown_entry(folder_path, POSTPROCESSING_OPERATION_NAME, pid, "start")


def log_postprocessing_end(folder_path: str, pid: int) -> None:
    """
    Log the end of the postprocessing operation.

    Args:
        folder_path: Path to the folder containing the CSV file
        pid: Process ID
    """
    _log_breakdown_entry(folder_path, POSTPROCESSING_OPERATION_NAME, pid, "end")

#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""
Reusable utilities for recording and reporting diagnosis information across benchmarks.

This module provides a standardized way to record failure diagnostics that can be
used by any benchmark in the benchpress suite.
"""

import fcntl
import json
import os
import socket
import sys
from datetime import datetime
from typing import Any, Dict, List, Optional


class DiagnosisRecorder:
    """
    Singleton class that records diagnosis information to a JSON file with append support.

    This class handles file locking to support concurrent writes from multiple
    benchmark instances. Only one instance exists per process.

    Example usage:
        recorder = DiagnosisRecorder.get_instance()
        recorder.record_failure(
            benchmark="tao_bench",
            error_type="port_unavailable",
            reason="Port 11211 is already in use",
            solutions=["Kill the process", "Use different port"],
            metadata={"port": 11211, "errno": 98}
        )
    """

    _instance: Optional["DiagnosisRecorder"] = None
    _lock = None  # Will be initialized as threading.Lock() when needed

    def __init__(
        self,
        diagnosis_file: Optional[str] = None,
        root_dir: Optional[str] = None,
        shared_file_path: Optional[str] = None,
    ):
        """
        Private constructor - use get_instance() instead.

        Args:
            diagnosis_file: Base name for the diagnosis file (default: failure_diagnosis)
            root_dir: Root directory for the diagnosis file (default: current directory)
            shared_file_path: Full path to an existing diagnosis file to use (for cross-process sharing)

        Note:
            If shared_file_path is provided, it takes precedence.
            Otherwise, a new file is created with a unique name containing process ID and timestamp
            to prevent conflicts between concurrent benchmark instances.
        """
        # If shared_file_path is provided, use it directly
        if shared_file_path:
            self.diagnosis_file_path = shared_file_path
            # Ensure the file exists
            if not os.path.exists(self.diagnosis_file_path):
                os.makedirs(
                    os.path.dirname(self.diagnosis_file_path) or ".", exist_ok=True
                )
                with open(self.diagnosis_file_path, "w") as f:
                    json.dump([], f)  # Initialize with empty array
            return

        # Otherwise, create a new unique file
        if root_dir is None:
            root_dir = os.getcwd()

        # Generate unique filename with process ID and timestamp
        pid = os.getpid()
        timestamp = datetime.utcnow().strftime("%Y%m%d_%H%M%S")

        if diagnosis_file is None:
            base_name = "failure_diagnosis"
        else:
            # Remove .json extension if provided
            base_name = diagnosis_file.replace(".json", "")

        unique_filename = f"{base_name}_{pid}_{timestamp}.json"
        self.diagnosis_file_path = os.path.join(root_dir, unique_filename)

        # Create the file immediately to claim it
        os.makedirs(os.path.dirname(self.diagnosis_file_path) or ".", exist_ok=True)
        with open(self.diagnosis_file_path, "w") as f:
            json.dump([], f)  # Initialize with empty array

    @classmethod
    def get_instance(
        cls,
        diagnosis_file: Optional[str] = None,
        root_dir: Optional[str] = None,
        shared_file_path: Optional[str] = None,
    ) -> "DiagnosisRecorder":
        """
        Get the singleton instance of DiagnosisRecorder.

        This method ensures only one DiagnosisRecorder instance exists per process.
        On first call, it creates the instance with the provided parameters.
        On subsequent calls, it returns the existing instance.

        Args:
            diagnosis_file: Base name for the diagnosis file (default: failure_diagnosis)
            root_dir: Root directory for the diagnosis file (default: current directory)
            shared_file_path: Full path to an existing diagnosis file to use (for cross-process sharing)

        Returns:
            The singleton DiagnosisRecorder instance

        Note:
            This method automatically manages the DIAGNOSIS_FILE_PATH environment variable
            for cross-process file sharing:
            - If the environment variable is already set (subprocess), uses that file path
            - If not set (parent process), creates instance and sets the environment variable
            This enables seamless file sharing between parent and child processes.
            Parameters are only used on the first call. Subsequent calls ignore parameters
            and return the existing instance.
        """
        if cls._instance is None:
            # Import threading here to avoid circular imports
            import threading

            if cls._lock is None:
                cls._lock = threading.Lock()

            with cls._lock:
                # Double-check locking pattern
                if cls._instance is None:
                    # Check environment variable for cross-process file sharing
                    if not shared_file_path:
                        env_file_path = os.environ.get("DIAGNOSIS_FILE_PATH")
                        if env_file_path:
                            # Subprocess: use shared file path from parent
                            shared_file_path = env_file_path

                    # Create the instance
                    cls._instance = cls(
                        diagnosis_file=diagnosis_file,
                        root_dir=root_dir,
                        shared_file_path=shared_file_path,
                    )

                    # Parent process: set environment variable for subprocesses
                    if not env_file_path:
                        os.environ["DIAGNOSIS_FILE_PATH"] = (
                            cls._instance.diagnosis_file_path
                        )

        return cls._instance

    @classmethod
    def reset_instance(cls) -> None:
        """
        Reset the singleton instance. Useful for testing.

        Warning:
            This should only be used in test code.
        """
        cls._instance = None

    def record_failure(
        self,
        benchmark: str,
        error_type: str,
        reason: str,
        solutions: Optional[List[str]] = None,
        metadata: Optional[Dict[str, Any]] = None,
    ) -> None:
        """
        Record a failure diagnosis entry.

        Args:
            benchmark: Name of the benchmark (e.g., "tao_bench", "feedsim")
            error_type: Type of error (e.g., "port_unavailable", "memory_error")
            reason: Human-readable description of the failure
            solutions: List of suggested solutions
            metadata: Additional metadata (port numbers, error codes, etc.)
        """
        entry = {
            "timestamp": datetime.utcnow().isoformat() + "Z",
            "benchmark": benchmark,
            "error_type": error_type,
            "reason": reason,
            "solutions": solutions or [],
            "metadata": metadata or {},
        }

        self._append_to_file(entry)

    def _append_to_file(self, entry: Dict[str, Any]) -> None:
        """
        Append an entry to the diagnosis file with file locking.

        Uses fcntl for file locking to handle concurrent writes.
        """
        # Ensure parent directory exists
        os.makedirs(os.path.dirname(self.diagnosis_file_path) or ".", exist_ok=True)

        # Open file with exclusive lock
        with open(self.diagnosis_file_path, "a+") as f:
            # Acquire exclusive lock
            fcntl.flock(f.fileno(), fcntl.LOCK_EX)

            try:
                # Read existing content
                f.seek(0)
                content = f.read().strip()

                if content:
                    try:
                        # Parse existing JSON array
                        records = json.loads(content)
                        if not isinstance(records, list):
                            records = [records]  # Convert single object to list
                    except json.JSONDecodeError:
                        # If file is corrupted, start fresh
                        records = []
                else:
                    # Empty file, start new array
                    records = []

                # Append new entry
                records.append(entry)

                # Write back to file
                f.seek(0)
                f.truncate()
                json.dump(records, f, indent=2)
                f.write("\n")  # Add trailing newline

            finally:
                # Release lock
                fcntl.flock(f.fileno(), fcntl.LOCK_UN)

    def read_all(self) -> List[Dict[str, Any]]:
        """
        Read all diagnosis records from the file.

        Returns:
            List of diagnosis records, or empty list if file doesn't exist
        """
        if not os.path.exists(self.diagnosis_file_path):
            return []

        with open(self.diagnosis_file_path, "r") as f:
            try:
                records = json.load(f)
                if isinstance(records, list):
                    return records
                else:
                    return [records]  # Convert single object to list
            except json.JSONDecodeError:
                return []

    def record_port_unavailable_error(
        self,
        port: int,
        benchmark: str,
        errno: int = 98,
    ) -> None:
        """
        Record a port unavailability error.

        Args:
            port: Port number that is unavailable
            benchmark: Name of the benchmark
            errno: OS error number (default: 98 for EADDRINUSE)
        """
        self.record_failure(
            benchmark=benchmark,
            error_type="port_unavailable",
            reason=f"Port {port} is already in use (either by another process or in TIME_WAIT/CLOSE_WAIT state)",
            solutions=[
                f"Kill the process using the port: lsof -i :{port} && kill -9 <PID>",
                "Choose a different port: Check job options available for setting port number",
                f"Wait 60-120 seconds for TIME_WAIT to clear: netstat -an | grep {port}",
            ],
            metadata={
                "port": port,
                "errno": errno,
            },
        )

    def merge_failure_to_results(
        self,
        results_dict: Dict[str, Any],
    ) -> None:
        """
        Read the diagnosis file and merge all failure information into the provided results dictionary.

        This is a convenience method for benchmark runners to easily add diagnosis
        information to their output. It merges all diagnosis records.

        Args:
            results_dict: Dictionary to merge failure information into

        Modifies:
            results_dict is updated with a "failures" key containing a list of all
            diagnosis records, or an empty list if no records are found.
        """
        try:
            # Read from the singleton instance's diagnosis file
            if not os.path.exists(self.diagnosis_file_path):
                results_dict["failures"] = []
                return

            with open(self.diagnosis_file_path, "r") as f:
                records = json.load(f)

                if not isinstance(records, list):
                    records = [records]

                # Add all failure records to results
                results_dict["failures"] = records
        except Exception as e:
            results_dict["failures"] = []
            results_dict["failure_read_error"] = f"Failed to read diagnosis file: {e}"


def check_port_available(
    port: int,
    interface: str = "0.0.0.0",
    benchmark: str = "unknown",
    root_dir: Optional[str] = None,
) -> bool:
    """
    Check if a port is available by attempting to bind to it.
    Raises SystemExit with helpful error message if port is not available.

    This approach handles both active processes and ports in TIME_WAIT/CLOSE_WAIT states
    that won't show up in lsof.

    NOTE: We do NOT use SO_REUSEADDR here because we want a strict check.
    If the port is in use by any process or in TIME_WAIT state, we should fail.

    Args:
        port: Port number to check
        interface: Network interface to bind to (default: "0.0.0.0" for all interfaces)
        benchmark: Name of the benchmark calling this function (for diagnosis recording)
        root_dir: Root directory for diagnosis file

    Raises:
        SystemExit: If the port is not available
    """
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as test_socket:
            # DO NOT set SO_REUSEADDR - we want strict port availability check
            test_socket.bind((interface, port))
        print(f"Port {port} is available")
        return True
    except OSError as e:
        # Record failure using the diagnosis utility
        record_port_unavailable_error(
            port=port,
            benchmark=benchmark,
            errno=e.errno,
            root_dir=root_dir,
        )

        # Print user-friendly error message
        error_msg = f"""
================================================================================
ERROR: Port {port} is not available
================================================================================

Port {port} is already in use (either by another process or in TIME_WAIT/CLOSE_WAIT state)

SOLUTIONS:
  1. Kill the process using the port: lsof -i :{port} && kill -9 <PID>
  2. Choose a different port: Check job options available for setting port number
  3. Wait 60-120 seconds for TIME_WAIT to clear: netstat -an | grep {port}

================================================================================
"""
        print(error_msg, file=sys.stderr)
        return False


def record_port_unavailable_error(
    port: int,
    benchmark: str,
    errno: int = 98,
    root_dir: Optional[str] = None,
) -> None:
    """
    Convenience function to record a port unavailability error.

    Args:
        port: Port number that is unavailable
        benchmark: Name of the benchmark
        errno: OS error number (default: 98 for EADDRINUSE)
        root_dir: Root directory for diagnosis file
    """
    recorder = DiagnosisRecorder.get_instance(root_dir=root_dir)
    recorder.record_port_unavailable_error(port=port, benchmark=benchmark, errno=errno)

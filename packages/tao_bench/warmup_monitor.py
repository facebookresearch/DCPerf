#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
import socket
import threading
import time
from parser import TaoBenchServerSnapshot


class WarmupMonitor:
    """Monitors server stats output to detect when warmup is complete.

    Warmup is considered complete when:
    1. Hit ratio >= hit_ratio_factor * target_hit_ratio
    2. QPS is stable (coefficient of variation < stability_threshold)
       over a rolling window of stability_window data points
    """

    def __init__(
        self,
        target_hit_ratio=0.9,
        stability_window=24,
        stability_threshold=0.05,
        hit_ratio_factor=0.95,
    ):
        self.target_hit_ratio = target_hit_ratio
        self.hit_threshold = hit_ratio_factor * target_hit_ratio
        self.stability_window = stability_window  # 24 * 5s = 2 minutes
        self.stability_threshold = stability_threshold
        self.qps_history = []
        self.is_warmed_up = False
        self._lock = threading.Lock()

    def process_line(self, line):
        """Process a server stats line. Returns True if warmup just completed."""
        snapshot = TaoBenchServerSnapshot(line)
        if not snapshot.valid:
            return False

        hit_rate = snapshot.get("hit_rate")
        total_qps = snapshot.get("fast_qps") + snapshot.get("slow_qps")

        with self._lock:
            if self.is_warmed_up:
                return False

            self.qps_history.append(total_qps)
            if len(self.qps_history) > self.stability_window:
                self.qps_history.pop(0)

            if hit_rate >= self.hit_threshold and self._is_qps_stable():
                self.is_warmed_up = True
                print(
                    f"[WarmupMonitor] Warmup complete: hit_rate={hit_rate:.4f} "
                    f"(threshold={self.hit_threshold:.4f}), "
                    f"QPS stable over {len(self.qps_history)} data points"
                )
                return True
        return False

    def _is_qps_stable(self):
        if len(self.qps_history) < self.stability_window:
            return False
        mean_qps = sum(self.qps_history) / len(self.qps_history)
        if mean_qps == 0:
            return False
        variance = sum((x - mean_qps) ** 2 for x in self.qps_history) / len(
            self.qps_history
        )
        cv = (variance**0.5) / mean_qps
        return cv < self.stability_threshold


class WarmupControlServer:
    """TCP server that responds to client warmup status polls.

    Clients connect and receive either "READY\\n" or "WAITING\\n".
    """

    def __init__(self, port, monitors):
        self.port = port
        self.monitors = monitors
        self.server_socket = None
        self._thread = None
        self.running = False

    def start(self):
        self.server_socket = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)
        self.server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server_socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        self.server_socket.settimeout(1.0)
        self.server_socket.bind(("::", self.port))
        self.server_socket.listen(32)
        self.running = True
        self._thread = threading.Thread(target=self._handle_connections, daemon=True)
        self._thread.start()
        print(f"[WarmupControlServer] Listening on port {self.port}")

    def _handle_connections(self):
        while self.running:
            try:
                conn, addr = self.server_socket.accept()
                try:
                    if all(m.is_warmed_up for m in self.monitors):
                        conn.sendall(b"READY\n")
                    else:
                        conn.sendall(b"WAITING\n")
                finally:
                    conn.close()
            except socket.timeout:
                continue
            except OSError:
                break

    def stop(self):
        self.running = False
        if self.server_socket:
            try:
                self.server_socket.close()
            except OSError:
                pass
        if self._thread:
            self._thread.join(timeout=5)
        print("[WarmupControlServer] Stopped")


class LogTailer:
    """Background thread that tails a log file and feeds lines to a WarmupMonitor."""

    def __init__(self, log_path, monitor, instance_id=0):
        self.log_path = log_path
        self.monitor = monitor
        self.instance_id = instance_id
        self._thread = None
        self.running = False

    def start(self):
        self.running = True
        self._thread = threading.Thread(target=self._tail, daemon=True)
        self._thread.start()

    def _tail(self):
        # Wait for file to appear
        while self.running and not os.path.exists(self.log_path):
            time.sleep(0.5)
        if not self.running:
            return

        with open(self.log_path, "r") as f:
            while self.running:
                line = f.readline()
                if line:
                    if self.monitor.process_line(line):
                        print(
                            f"[LogTailer] Server instance {self.instance_id} "
                            f"warmup complete"
                        )
                else:
                    # No new data yet
                    if self.monitor.is_warmed_up:
                        # Stop tailing once warmed up
                        return
                    time.sleep(0.5)

    def stop(self):
        self.running = False
        if self._thread:
            self._thread.join(timeout=5)


def poll_control_port(hostname, port, timeout=5):
    """Poll the warmup control server. Returns True if server reports READY."""
    try:
        with socket.create_connection((hostname, port), timeout=timeout) as s:
            data = s.recv(64).decode().strip()
            return data == "READY"
    except OSError:
        return False

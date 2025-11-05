#!/usr/bin/env python3
"""
Start Django server with HAProxy load balancer and multiple Proxygen workers.

This script orchestrates the startup of:
1. Multiple Proxygen worker processes on different ports
2. HAProxy load balancer distributing traffic across workers

Usage:
    python start_loadbalanced_server.py [options]

Example:
    # Start with 8 workers (default)
    python start_loadbalanced_server.py

    # Start with 16 workers
    python start_loadbalanced_server.py --workers 16

    # Custom HAProxy config
    python start_loadbalanced_server.py --haproxy-config custom_haproxy.cfg
"""

import argparse
import logging
import os
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import IO, List, Optional

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format="[%(asctime)s] [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)
logger = logging.getLogger(__name__)


class UWSGIManager:
    """Manages uWSGI with Proxygen workers and HAProxy load balancer."""

    def __init__(
        self,
        num_workers: int = 8,
        base_port: int = 8001,
        lb_port: int = 8000,
        stats_port: int = 8080,
        threads_per_worker: int = 14,
        haproxy_config: Optional[str] = None,
        uwsgi_config: Optional[str] = None,
        log_dir: Optional[str] = None,
    ):
        """
        Initialize uWSGI manager.

        Args:
            num_workers: Number of uWSGI worker processes
            base_port: Starting port for workers (8001, 8002, ...)
            lb_port: Load balancer frontend port
            stats_port: HAProxy stats dashboard port
            threads_per_worker: Threads per Proxygen worker
            haproxy_config: Path to HAProxy config (auto-generated if None)
            uwsgi_config: Path to uWSGI config (default: uwsgi_loadbalanced.ini)
            log_dir: Directory for log files (None = logs to stdout only)
        """
        self.num_workers = num_workers
        self.base_port = base_port
        self.lb_port = lb_port
        self.stats_port = stats_port
        self.threads_per_worker = threads_per_worker
        self.haproxy_config = haproxy_config
        self.uwsgi_config = uwsgi_config
        self.log_dir = Path(log_dir) if log_dir else None

        # Process tracking
        self.uwsgi_process: Optional[subprocess.Popen] = None
        self.haproxy_process: Optional[subprocess.Popen] = None

        # Log file handles
        self.log_files: List[IO] = []

        # Log streaming threads
        self.log_threads: List[threading.Thread] = []
        self.stop_logging = threading.Event()

        # Directory setup
        self.script_dir = Path(__file__).parent
        self.default_uwsgi_config = self.script_dir / "uwsgi_loadbalanced.ini"

        # Create log directory if specified
        if self.log_dir:
            self.log_dir.mkdir(parents=True, exist_ok=True)
            logger.info(f"Logs will be saved to: {self.log_dir}")

        # Validate prerequisites
        self._validate_setup()

    def start_uwsgi(self) -> None:
        """Start uWSGI with workers."""
        # Determine config path
        if self.uwsgi_config:
            config_path = Path(self.uwsgi_config)
            if not config_path.exists():
                raise FileNotFoundError(f"uWSGI config not found: {config_path}")
        else:
            config_path = self.default_uwsgi_config
            if not config_path.exists():
                raise FileNotFoundError(
                    f"Default uWSGI config not found: {config_path}"
                )

        logger.info(f"Starting uWSGI with config: {config_path}")

        # Prepare environment variables
        env = os.environ.copy()
        env["PROXYGEN_BASE_PORT"] = str(self.base_port)
        env["PROXYGEN_THREADS"] = str(self.threads_per_worker)
        env["PYTHONUNBUFFERED"] = "1"

        # Start uWSGI with config overrides
        # Use --set to override config variables (standard uWSGI way)
        self.uwsgi_process = subprocess.Popen(
            [
                "uwsgi",
                "--ini",
                str(config_path),
                "--set",
                f"workers={self.num_workers}",
            ],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )

        logger.info(
            f"uWSGI started (PID: {self.uwsgi_process.pid})\n"
            f"  Workers: {self.num_workers}\n"
            f"  Ports: {self.base_port}-{self.base_port + self.num_workers - 1}\n"
            f"  Threads per worker: {self.threads_per_worker}"
        )

        # Start log streaming thread for uWSGI
        log_file = None
        if self.log_dir:
            log_file_path = self.log_dir / "uwsgi.log"
            log_file = open(log_file_path, "w")
            self.log_files.append(log_file)

        log_thread = threading.Thread(
            target=self._stream_logs,
            args=(self.uwsgi_process, "[uWSGI]", log_file),
            daemon=True,
        )
        log_thread.start()
        self.log_threads.append(log_thread)

    def stop(self) -> None:
        """Stop all processes (uWSGI + HAProxy)."""
        logger.info("Shutting down...")

        # Signal log threads to stop
        self.stop_logging.set()

        # Stop HAProxy
        if self.haproxy_process:
            logger.info("Stopping HAProxy...")
            self.haproxy_process.terminate()
            try:
                self.haproxy_process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.haproxy_process.kill()

        # Stop uWSGI
        if self.uwsgi_process:
            logger.info("Stopping uWSGI (will wait for workers to finish)...")
            self.uwsgi_process.terminate()
            try:
                self.uwsgi_process.wait(timeout=30)  # Longer timeout for uWSGI
            except subprocess.TimeoutExpired:
                logger.warning("uWSGI did not stop gracefully, killing...")
                self.uwsgi_process.kill()

        # Wait for log threads to finish
        for thread in self.log_threads:
            thread.join(timeout=2)

        # Close log files
        for log_file in self.log_files:
            try:
                log_file.close()
            except Exception as e:
                logger.warning(f"Error closing log file: {e}")

        logger.info("All processes stopped")

        if self.log_dir:
            logger.info(f"Logs saved to: {self.log_dir}")

    def _stream_logs(
        self, process: subprocess.Popen, prefix: str, log_file: Optional[IO] = None
    ) -> None:
        """Stream logs from a process to stdout and optionally to a file."""
        try:
            for line in iter(process.stdout.readline, ""):
                if self.stop_logging.is_set():
                    break
                if line:
                    line = line.rstrip()
                    print(f"{prefix} {line}", flush=True)
                    if log_file:
                        log_file.write(f"{prefix} {line}\n")
                        log_file.flush()
        except Exception as e:
            logger.error(f"Error streaming logs for {prefix}: {e}")

    def _check_port_available(self, port: int) -> bool:
        """Check if a port is available for binding."""
        import socket

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.bind(("0.0.0.0", port))
            sock.close()
            return True
        except OSError:
            return False

    def _find_process_on_port(self, port: int) -> Optional[str]:
        """Find what process is using a port."""
        try:
            result = subprocess.run(
                ["lsof", "-i", f":{port}", "-t"],
                capture_output=True,
                text=True,
            )
            if result.returncode == 0 and result.stdout.strip():
                pid = result.stdout.strip().split()[0]
                proc_result = subprocess.run(
                    ["ps", "-p", pid, "-o", "comm="],
                    capture_output=True,
                    text=True,
                )
                if proc_result.returncode == 0:
                    return f"PID {pid} ({proc_result.stdout.strip()})"
            return None
        except Exception:
            return None

    def _validate_setup(self) -> None:
        """Validate that required files and commands exist."""
        # Check for uWSGI
        if (
            subprocess.run(
                ["which", "uwsgi"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            ).returncode
            != 0
        ):
            logger.warning(
                "uWSGI not found in PATH. Install with:\n" "  pip install uwsgi"
            )
            raise RuntimeError("uWSGI not installed")

        # Check for HAProxy
        if (
            subprocess.run(
                ["which", "haproxy"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            ).returncode
            != 0
        ):
            logger.warning(
                "HAProxy not found in PATH. Install with:\n"
                "  Ubuntu/Debian: sudo apt-get install haproxy\n"
                "  CentOS/RHEL: sudo yum install haproxy\n"
                "  macOS: brew install haproxy"
            )
            raise RuntimeError("HAProxy not installed")

        # Check port availability
        ports_to_check = [self.lb_port, self.stats_port] + [
            self.base_port + i for i in range(self.num_workers)
        ]

        unavailable_ports = []
        for port in ports_to_check:
            if not self._check_port_available(port):
                process_info = self._find_process_on_port(port)
                if process_info:
                    unavailable_ports.append(f"  Port {port}: {process_info}")
                else:
                    unavailable_ports.append(f"  Port {port}: unknown process")

        if unavailable_ports:
            logger.error("The following ports are already in use:")
            for info in unavailable_ports:
                logger.error(info)
            logger.error("\nTo fix this:")
            logger.error("1. Kill the processes: pkill -f haproxy; pkill -f uwsgi")
            logger.error(
                "2. Or use different ports: --lb-port 9000 --base-port 9001 --stats-port 9080"
            )
            raise RuntimeError("Required ports are not available")

    def generate_haproxy_config(self) -> Path:
        """Generate HAProxy configuration dynamically."""
        config_path = self.script_dir / "haproxy_generated.cfg"

        # Generate server entries for all workers
        server_lines = []
        for i in range(1, self.num_workers + 1):
            port = self.base_port + (i - 1)
            server_lines.append(
                f"    server worker{i} 127.0.0.1:{port} "
                f"check weight 1 maxconn 10000 inter 2s fall 3 rise 2"
            )

        servers_config = "\n".join(server_lines)

        # HAProxy configuration template
        config_content = f"""global
    log stdout format raw local0 info
    maxconn 100000
    nbthread 4
    tune.bufsize 32768
    tune.maxrewrite 1024

defaults
    log global
    mode http
    option httplog
    option dontlognull
    option http-server-close
    option forwardfor except 127.0.0.0/8
    option redispatch
    retries 3
    timeout connect 5000ms
    timeout client 50000ms
    timeout server 50000ms
    timeout http-keep-alive 10s
    timeout check 2000ms

frontend django_frontend
    bind *:{self.lb_port}
    maxconn 50000
    default_backend django_workers
    option dontlog-normal

backend django_workers
    balance leastconn
    option httpchk GET /
    http-check expect status 200
    timeout connect 3000ms
    timeout server 30000ms

{servers_config}

listen stats
    bind *:{self.stats_port}
    stats enable
    stats uri /stats
    stats refresh 5s
    stats show-legends
    stats show-node
    stats admin if TRUE
"""

        # Write configuration
        with open(config_path, "w") as f:
            f.write(config_content)

        logger.info(f"Generated HAProxy config: {config_path}")
        return config_path

    def wait_for_workers_ready(self, timeout: int = 30) -> bool:
        """Wait for all workers to be ready to accept connections."""
        import socket

        logger.info("Waiting for workers to be ready...")
        start_time = time.time()

        while time.time() - start_time < timeout:
            all_ready = True

            for i in range(self.num_workers):
                port = self.base_port + i
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(0.5)

                try:
                    result = sock.connect_ex(("127.0.0.1", port))
                    if result != 0:
                        all_ready = False
                except Exception:
                    all_ready = False
                finally:
                    sock.close()

            if all_ready:
                logger.info("All workers are ready!")
                return True

            time.sleep(1)

        logger.warning(f"Timeout waiting for workers (waited {timeout}s)")
        return False

    def start_haproxy(self) -> None:
        """Start HAProxy load balancer."""
        # Generate or use provided config
        if self.haproxy_config:
            config_path = Path(self.haproxy_config)
            if not config_path.exists():
                raise FileNotFoundError(f"HAProxy config not found: {config_path}")
        else:
            config_path = self.generate_haproxy_config()

        logger.info(f"Starting HAProxy with config: {config_path}")

        # Start HAProxy
        self.haproxy_process = subprocess.Popen(
            ["haproxy", "-f", str(config_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )

        logger.info(
            f"HAProxy started (PID: {self.haproxy_process.pid})\n"
            f"  Frontend: http://127.0.0.1:{self.lb_port}\n"
            f"  Stats: http://127.0.0.1:{self.stats_port}/stats"
        )

        # Start log streaming thread for HAProxy
        log_file = None
        if self.log_dir:
            log_file_path = self.log_dir / "haproxy.log"
            log_file = open(log_file_path, "w")
            self.log_files.append(log_file)

        log_thread = threading.Thread(
            target=self._stream_logs,
            args=(self.haproxy_process, "[HAProxy]", log_file),
            daemon=True,
        )
        log_thread.start()
        self.log_threads.append(log_thread)

    def start(self) -> None:
        """Start all components (uWSGI + HAProxy)."""
        try:
            # Start uWSGI
            self.start_uwsgi()

            # Wait for workers to be ready
            if not self.wait_for_workers_ready():
                logger.warning("Some workers may not be ready yet, continuing anyway")

            # Start HAProxy
            self.start_haproxy()

            logger.info(
                "\n" + "=" * 60 + "\n"
                f"Load-balanced Django server with uWSGI is running!\n"
                f"  Access via: http://127.0.0.1:{self.lb_port}\n"
                f"  uWSGI workers: {self.num_workers} (ports {self.base_port}-{self.base_port + self.num_workers - 1})\n"
                f"  Stats UI: http://127.0.0.1:{self.stats_port}/stats\n"
                f"  Threads per worker: {self.threads_per_worker}\n"
                "\n"
                f"Press Ctrl+C to stop all processes\n" + "=" * 60
            )

        except Exception as e:
            logger.error(f"Failed to start server: {e}")
            self.stop()
            raise

    def run(self) -> None:
        """Start server and block until interrupted."""

        # Setup signal handlers
        def signal_handler(signum, frame):
            logger.info(f"\nReceived signal {signum}")
            self.stop()
            sys.exit(0)

        signal.signal(signal.SIGINT, signal_handler)
        signal.signal(signal.SIGTERM, signal_handler)

        # Start everything
        self.start()

        # Monitor processes
        try:
            while True:
                # Check if any process died
                if self.haproxy_process and self.haproxy_process.poll() is not None:
                    logger.error("HAProxy died unexpectedly!")
                    break

                if self.uwsgi_process and self.uwsgi_process.poll() is not None:
                    logger.error("uWSGI died unexpectedly!")
                    break

                time.sleep(1)
        except KeyboardInterrupt:
            pass
        finally:
            self.stop()


class ServerManager:
    """Manages Proxygen worker processes and HAProxy load balancer."""

    def __init__(
        self,
        num_workers: int = 8,
        base_port: int = 8001,
        lb_port: int = 8000,
        stats_port: int = 8080,
        threads_per_worker: int = 8,
        haproxy_config: Optional[str] = None,
        log_dir: Optional[str] = None,
    ):
        """
        Initialize server manager.

        Args:
            num_workers: Number of Proxygen worker processes
            base_port: Starting port for workers (8001, 8002, ...)
            lb_port: Load balancer frontend port
            stats_port: HAProxy stats dashboard port
            threads_per_worker: Threads per Proxygen worker
            haproxy_config: Path to HAProxy config (auto-generated if None)
            log_dir: Directory for log files (None = logs to stdout only)
        """
        self.num_workers = num_workers
        self.base_port = base_port
        self.lb_port = lb_port
        self.stats_port = stats_port
        self.threads_per_worker = threads_per_worker
        self.haproxy_config = haproxy_config
        self.log_dir = Path(log_dir) if log_dir else None

        # Process tracking
        self.worker_processes: List[subprocess.Popen] = []
        self.haproxy_process: Optional[subprocess.Popen] = None

        # Log file handles
        self.log_files: List[IO] = []

        # Log streaming threads
        self.log_threads: List[threading.Thread] = []
        self.stop_logging = threading.Event()

        # Directory setup
        self.script_dir = Path(__file__).parent
        self.worker_script = self.script_dir / "proxygen_wsgi.py"

        # Create log directory if specified
        if self.log_dir:
            self.log_dir.mkdir(parents=True, exist_ok=True)
            logger.info(f"Logs will be saved to: {self.log_dir}")

        # Validate prerequisites
        self._validate_setup()

    def _check_port_available(self, port: int) -> bool:
        """Check if a port is available for binding."""
        import socket

        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            sock.bind(("0.0.0.0", port))
            sock.close()
            return True
        except OSError:
            return False

    def _find_process_on_port(self, port: int) -> Optional[str]:
        """Find what process is using a port."""
        try:
            result = subprocess.run(
                ["lsof", "-i", f":{port}", "-t"],
                capture_output=True,
                text=True,
            )
            if result.returncode == 0 and result.stdout.strip():
                pid = result.stdout.strip().split()[0]
                # Get process name
                proc_result = subprocess.run(
                    ["ps", "-p", pid, "-o", "comm="],
                    capture_output=True,
                    text=True,
                )
                if proc_result.returncode == 0:
                    return f"PID {pid} ({proc_result.stdout.strip()})"
            return None
        except Exception:
            return None

    def _validate_setup(self) -> None:
        """Validate that required files and commands exist."""
        if not self.worker_script.exists():
            raise FileNotFoundError(
                f"Worker script not found: {self.worker_script}\n"
                f"Expected: {self.script_dir}/proxygen_wsgi.py"
            )

        # Check for HAProxy
        if (
            subprocess.run(
                ["which", "haproxy"],
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            ).returncode
            != 0
        ):
            logger.warning(
                "HAProxy not found in PATH. Install with:\n"
                "  Ubuntu/Debian: sudo apt-get install haproxy\n"
                "  CentOS/RHEL: sudo yum install haproxy\n"
                "  macOS: brew install haproxy"
            )
            raise RuntimeError("HAProxy not installed")

        # Check port availability
        ports_to_check = [self.lb_port, self.stats_port] + [
            self.base_port + i for i in range(self.num_workers)
        ]

        unavailable_ports = []
        for port in ports_to_check:
            if not self._check_port_available(port):
                process_info = self._find_process_on_port(port)
                if process_info:
                    unavailable_ports.append(f"  Port {port}: {process_info}")
                else:
                    unavailable_ports.append(f"  Port {port}: unknown process")

        if unavailable_ports:
            logger.error("The following ports are already in use:")
            for info in unavailable_ports:
                logger.error(info)
            logger.error("\nTo fix this:")
            logger.error(
                "1. Kill the processes: pkill -f haproxy; pkill -f proxygen_wsgi"
            )
            logger.error(
                "2. Or use different ports: --lb-port 9000 --base-port 9001 --stats-port 9080"
            )
            raise RuntimeError("Required ports are not available")

    def generate_haproxy_config(self) -> Path:
        """Generate HAProxy configuration dynamically."""
        config_path = self.script_dir / "haproxy_generated.cfg"

        # Generate server entries for all workers
        server_lines = []
        for i in range(1, self.num_workers + 1):
            port = self.base_port + (i - 1)
            server_lines.append(
                f"    server worker{i} 127.0.0.1:{port} "
                f"check weight 1 maxconn 10000 inter 2s fall 3 rise 2"
            )

        servers_config = "\n".join(server_lines)

        # HAProxy configuration template
        config_content = f"""global
    log stdout format raw local0 info
    maxconn 100000
    nbthread 4
    tune.bufsize 32768
    tune.maxrewrite 1024

defaults
    log global
    mode http
    option httplog
    option dontlognull
    option http-server-close
    option forwardfor except 127.0.0.0/8
    option redispatch
    retries 3
    timeout connect 5000ms
    timeout client 50000ms
    timeout server 50000ms
    timeout http-keep-alive 10s
    timeout check 2000ms

frontend django_frontend
    bind *:{self.lb_port}
    maxconn 50000
    default_backend django_workers
    option dontlog-normal

backend django_workers
    balance leastconn
    option httpchk GET /
    http-check expect status 200
    timeout connect 3000ms
    timeout server 30000ms

{servers_config}

listen stats
    bind *:{self.stats_port}
    stats enable
    stats uri /stats
    stats refresh 5s
    stats show-legends
    stats show-node
    stats admin if TRUE
"""

        # Write configuration
        with open(config_path, "w") as f:
            f.write(config_content)

        logger.info(f"Generated HAProxy config: {config_path}")
        return config_path

    def _stream_logs(
        self, process: subprocess.Popen, prefix: str, log_file: Optional[IO] = None
    ) -> None:
        """
        Stream logs from a process to stdout and optionally to a file.

        Args:
            process: Process to stream logs from
            prefix: Prefix for log lines (e.g., "[Worker-1]")
            log_file: Optional file handle to write logs to
        """
        try:
            for line in iter(process.stdout.readline, ""):
                if self.stop_logging.is_set():
                    break

                if line:
                    # Remove trailing newline
                    line = line.rstrip()

                    # Print to stdout
                    print(f"{prefix} {line}", flush=True)

                    # Write to log file if provided
                    if log_file:
                        log_file.write(f"{prefix} {line}\n")
                        log_file.flush()
        except Exception as e:
            logger.error(f"Error streaming logs for {prefix}: {e}")

    def start_worker(self, worker_id: int, port: int) -> subprocess.Popen:
        """Start a single Proxygen worker process."""
        logger.info(f"Starting worker {worker_id} on port {port}...")

        env = os.environ.copy()
        env["PYTHONUNBUFFERED"] = "1"  # Disable output buffering

        # Start worker process
        process = subprocess.Popen(
            [
                sys.executable,
                str(self.worker_script),
                "--port",
                str(port),
                "--threads",
                str(self.threads_per_worker),
            ],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,  # Line buffered
        )

        logger.info(f"Worker {worker_id} started (PID: {process.pid})")

        # Start log streaming thread
        log_file = None
        if self.log_dir:
            log_file_path = self.log_dir / f"worker_{worker_id}.log"
            log_file = open(log_file_path, "w")
            self.log_files.append(log_file)

        log_thread = threading.Thread(
            target=self._stream_logs,
            args=(process, f"[Worker-{worker_id}]", log_file),
            daemon=True,
        )
        log_thread.start()
        self.log_threads.append(log_thread)

        return process

    def start_workers(self) -> None:
        """Start all Proxygen worker processes."""
        logger.info(f"Starting {self.num_workers} Proxygen workers...")

        for i in range(1, self.num_workers + 1):
            port = self.base_port + (i - 1)
            process = self.start_worker(i, port)
            self.worker_processes.append(process)

            # Brief delay to avoid port conflicts
            time.sleep(0.2)

        logger.info(f"All {self.num_workers} workers started")

    def wait_for_workers_ready(self, timeout: int = 30) -> bool:
        """Wait for all workers to be ready to accept connections."""
        import socket

        logger.info("Waiting for workers to be ready...")
        start_time = time.time()

        while time.time() - start_time < timeout:
            all_ready = True

            for i in range(self.num_workers):
                port = self.base_port + i
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(0.5)

                try:
                    result = sock.connect_ex(("127.0.0.1", port))
                    if result != 0:
                        all_ready = False
                except Exception:
                    all_ready = False
                finally:
                    sock.close()

            if all_ready:
                logger.info("All workers are ready!")
                return True

            time.sleep(1)

        logger.warning(f"Timeout waiting for workers (waited {timeout}s)")
        return False

    def start_haproxy(self) -> None:
        """Start HAProxy load balancer."""
        # Generate or use provided config
        if self.haproxy_config:
            config_path = Path(self.haproxy_config)
            if not config_path.exists():
                raise FileNotFoundError(f"HAProxy config not found: {config_path}")
        else:
            config_path = self.generate_haproxy_config()

        logger.info(f"Starting HAProxy with config: {config_path}")

        # Start HAProxy
        self.haproxy_process = subprocess.Popen(
            ["haproxy", "-f", str(config_path)],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )

        logger.info(
            f"HAProxy started (PID: {self.haproxy_process.pid})\n"
            f"  Frontend: http://127.0.0.1:{self.lb_port}\n"
            f"  Stats: http://127.0.0.1:8080/stats"
        )

        # Start log streaming thread for HAProxy
        log_file = None
        if self.log_dir:
            log_file_path = self.log_dir / "haproxy.log"
            log_file = open(log_file_path, "w")
            self.log_files.append(log_file)

        log_thread = threading.Thread(
            target=self._stream_logs,
            args=(self.haproxy_process, "[HAProxy]", log_file),
            daemon=True,
        )
        log_thread.start()
        self.log_threads.append(log_thread)

    def start(self) -> None:
        """Start all components (workers + load balancer)."""
        try:
            # Start workers
            self.start_workers()

            # Wait for workers to be ready
            if not self.wait_for_workers_ready():
                logger.warning("Some workers may not be ready yet, continuing anyway")

            # Start HAProxy
            self.start_haproxy()

            logger.info(
                "\n" + "=" * 60 + "\n"
                f"Load-balanced Django server is running!\n"
                f"  Access via: http://127.0.0.1:{self.lb_port}\n"
                f"  Workers: {self.num_workers} (ports {self.base_port}-{self.base_port + self.num_workers - 1})\n"
                f"  Stats UI: http://127.0.0.1:8080/stats\n"
                f"  Threads per worker: {self.threads_per_worker}\n"
                "\n"
                f"Press Ctrl+C to stop all processes\n" + "=" * 60
            )

        except Exception as e:
            logger.error(f"Failed to start server: {e}")
            self.stop()
            raise

    def _stop_haproxy(self) -> None:
        """Stop HAProxy process gracefully."""
        if not self.haproxy_process:
            return

        logger.info("Stopping HAProxy...")
        self.haproxy_process.terminate()
        try:
            self.haproxy_process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.haproxy_process.kill()

    def _stop_workers(self) -> None:
        """Stop all worker processes gracefully."""
        if not self.worker_processes:
            return

        logger.info(f"Stopping {len(self.worker_processes)} workers...")

        # Terminate all workers
        for i, process in enumerate(self.worker_processes, 1):
            logger.info(f"Stopping worker {i} (PID: {process.pid})...")
            process.terminate()

        # Wait for graceful shutdown
        for process in self.worker_processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()

    def _stop_log_threads(self) -> None:
        """Stop all log streaming threads."""
        for thread in self.log_threads:
            thread.join(timeout=2)

    def _close_log_files(self) -> None:
        """Close all open log files."""
        for log_file in self.log_files:
            try:
                log_file.close()
            except Exception as e:
                logger.warning(f"Error closing log file: {e}")

    def stop(self) -> None:
        """Stop all processes (workers + load balancer)."""
        logger.info("Shutting down...")

        # Signal log threads to stop
        self.stop_logging.set()

        # Stop components
        self._stop_haproxy()
        self._stop_workers()
        self._stop_log_threads()
        self._close_log_files()

        logger.info("All processes stopped")

        if self.log_dir:
            logger.info(f"Logs saved to: {self.log_dir}")

    def run(self) -> None:
        """Start server and block until interrupted."""

        # Setup signal handlers
        def signal_handler(signum, frame):
            logger.info(f"\nReceived signal {signum}")
            self.stop()
            sys.exit(0)

        signal.signal(signal.SIGINT, signal_handler)
        signal.signal(signal.SIGTERM, signal_handler)

        # Start everything
        self.start()

        # Monitor processes
        try:
            while True:
                # Check if any process died
                if self.haproxy_process and self.haproxy_process.poll() is not None:
                    logger.error("HAProxy died unexpectedly!")
                    break

                for i, process in enumerate(self.worker_processes, 1):
                    if process.poll() is not None:
                        logger.error(f"Worker {i} died unexpectedly!")
                        break

                time.sleep(1)
        except KeyboardInterrupt:
            pass
        finally:
            self.stop()


def main():
    parser = argparse.ArgumentParser(
        description="Start load-balanced Django server with HAProxy and Proxygen workers",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )

    parser.add_argument(
        "--workers",
        type=int,
        default=8,
        help="Number of Proxygen worker processes",
    )

    parser.add_argument(
        "--base-port",
        type=int,
        default=8001,
        help="Starting port for workers (workers use base_port, base_port+1, ...)",
    )

    parser.add_argument(
        "--lb-port",
        type=int,
        default=8000,
        help="Load balancer frontend port",
    )

    parser.add_argument(
        "--stats-port",
        type=int,
        default=8080,
        help="HAProxy stats dashboard port",
    )

    parser.add_argument(
        "--threads-per-worker",
        type=int,
        default=8,
        help="Number of threads per Proxygen worker",
    )

    parser.add_argument(
        "--haproxy-config",
        type=str,
        default=None,
        help="Path to custom HAProxy config (auto-generated if not provided)",
    )

    parser.add_argument(
        "--log-dir",
        type=str,
        default=None,
        help="Directory to save log files (default: logs only to stdout)",
    )

    parser.add_argument(
        "--use-uwsgi",
        action="store_true",
        help="Use uWSGI to manage workers (instead of direct Python processes)",
    )

    parser.add_argument(
        "--uwsgi-config",
        type=str,
        default=None,
        help="Path to uWSGI config file (default: uwsgi_loadbalanced.ini)",
    )

    args = parser.parse_args()

    # Choose between uWSGI mode or direct worker mode
    if args.use_uwsgi:
        # Use uWSGI to manage workers
        manager = UWSGIManager(
            num_workers=args.workers,
            base_port=args.base_port,
            lb_port=args.lb_port,
            stats_port=args.stats_port,
            threads_per_worker=args.threads_per_worker,
            haproxy_config=args.haproxy_config,
            uwsgi_config=args.uwsgi_config,
            log_dir=args.log_dir,
        )
    else:
        # Direct Python worker processes
        manager = ServerManager(
            num_workers=args.workers,
            base_port=args.base_port,
            lb_port=args.lb_port,
            stats_port=args.stats_port,
            threads_per_worker=args.threads_per_worker,
            haproxy_config=args.haproxy_config,
            log_dir=args.log_dir,
        )

    manager.run()


if __name__ == "__main__":
    main()

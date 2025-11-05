#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""
Proxygen-based WSGI entry point for DjangoBench

This module initializes Proxygen HTTP server within each uWSGI worker process.
uWSGI handles:
  - Worker process forking and management
  - Memory leak cleanup (harakiri, reload-on-rss)
  - Process monitoring

Proxygen handles:
  - Asynchronous HTTP request processing
  - Multi-threaded request handling
  - Connection management

Architecture:
  uWSGI (process manager)
    └─> Worker Process (fork)
        └─> Proxygen Server (async HTTP)
            └─> Django ASGI App
"""

import logging
import os
import signal
import sys
import threading

# Add proxygen_binding to path if not installed system-wide
PROXYGEN_BINDING_PATH = os.path.join(
    os.path.dirname(os.path.dirname(os.path.dirname(__file__))), "proxygen_binding"
)
if os.path.exists(PROXYGEN_BINDING_PATH) and PROXYGEN_BINDING_PATH not in sys.path:
    sys.path.insert(0, PROXYGEN_BINDING_PATH)

try:
    from django_asgi_adapter import create_django_handler
    from proxygen_binding import init_logging, ProxygenServer
except ImportError as e:
    print(f"ERROR: Failed to import proxygen_binding: {e}", file=sys.stderr)
    print("Make sure proxygen_binding is built and available", file=sys.stderr)
    print(f"Expected path: {PROXYGEN_BINDING_PATH}", file=sys.stderr)
    sys.exit(1)

# Configure Django settings
os.environ.setdefault("DJANGO_SETTINGS_MODULE", "django_workload.settings")

# Initialize logging
logging.basicConfig(
    level=logging.INFO,
    format="[%(process)d] %(asctime)s - %(name)s - %(levelname)s - %(message)s",
)
logger = logging.getLogger(__name__)

# Global server instance for this worker process
_proxygen_server = None
_server_thread = None
_shutdown_event = threading.Event()


def start_proxygen_server(port=None, threads=None):
    """
    Start Proxygen server in this uWSGI worker process or standalone.

    This function is called by uWSGI's postfork hook to start Proxygen
    in each worker process after forking, or can be called directly with
    explicit port and threads arguments for load-balanced setups.

    Args:
        port: Port to listen on (overrides environment variable)
        threads: Number of threads (overrides environment variable, 0=auto-detect)

    Architecture:
        Proxygen RequestData
            ↓
        django_asgi_adapter.ASGIRequestHandler (converts to ASGI protocol)
            ↓
        django.core.asgi.get_asgi_application() (Django's ASGI app)
            ↓
        Django views/middleware
    """
    global _proxygen_server, _server_thread

    # Get configuration from environment or arguments
    ip = os.environ.get("PROXYGEN_IP", "0.0.0.0")
    port = port if port is not None else int(os.environ.get("PROXYGEN_PORT", "8000"))
    threads = (
        threads if threads is not None else int(os.environ.get("PROXYGEN_THREADS", "0"))
    )  # 0 = auto-detect

    logger.info(
        "Initializing Proxygen server in worker %d: %s:%d with %d threads",
        os.getpid(),
        ip,
        port,
        threads,
    )

    try:
        # Initialize Proxygen logging
        init_logging()

        # Create Django ASGI handler
        # This internally calls django.core.asgi.get_asgi_application()
        # and wraps it with ASGIRequestHandler to convert between
        # Proxygen's RequestData/ResponseData and Django's ASGI protocol
        django_handler = create_django_handler()
        logger.info(
            "Django ASGI handler created (using django.core.asgi.get_asgi_application)"
        )

        # Create Proxygen server with async callback
        # Using async_callback enables true concurrent request processing
        # where multiple requests can be handled simultaneously per thread
        _proxygen_server = ProxygenServer(
            ip=ip,
            port=port,
            threads=threads,
            async_callback=django_handler,
            is_async=True,
        )

        # Start server
        _proxygen_server.start()
        logger.info(
            "Proxygen server started in worker %d on %s:%d", os.getpid(), ip, port
        )

    except Exception as e:
        logger.exception(
            "Failed to start Proxygen server in worker %d: %s", os.getpid(), e
        )
        # Don't exit - let uWSGI handle worker failure
        raise


def stop_proxygen_server():
    """
    Stop Proxygen server gracefully.

    This function is called when the worker is shutting down.
    """
    global _proxygen_server

    if _proxygen_server:
        logger.info("Stopping Proxygen server in worker %d", os.getpid())
        try:
            _proxygen_server.stop()
            logger.info("Proxygen server stopped in worker %d", os.getpid())
        except Exception as e:
            logger.exception(
                "Error stopping Proxygen server in worker %d: %s", os.getpid(), e
            )
        finally:
            _proxygen_server = None


def signal_handler(signum, frame):
    """Handle shutdown signals"""
    logger.info("Worker %d received signal %d, shutting down", os.getpid(), signum)
    stop_proxygen_server()
    sys.exit(0)


# Standalone mode - for load balanced setups or testing
if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(
        description="Start Proxygen-based Django server",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument(
        "--port",
        type=int,
        default=8000,
        help="Port to listen on",
    )
    parser.add_argument(
        "--threads",
        type=int,
        default=0,
        help="Number of threads (0 = auto-detect based on CPU cores)",
    )

    args = parser.parse_args()

    start_proxygen_server(port=args.port, threads=args.threads)

    try:
        # Keep process alive
        if _proxygen_server:
            _proxygen_server.wait()
    except KeyboardInterrupt:
        logger.info("Received Ctrl+C, shutting down")
        stop_proxygen_server()


# Initialize when module is loaded (works with lazy-apps)
try:
    import uwsgi

    # Check if we're in a worker process (worker_id >= 1)
    # In master process, worker_id is 0
    if uwsgi.worker_id() > 0:
        logger.info(
            "Detected uWSGI worker %d, starting Proxygen server", uwsgi.worker_id()
        )

        # Register signal handlers
        signal.signal(signal.SIGTERM, signal_handler)
        signal.signal(signal.SIGINT, signal_handler)

        # Start Proxygen server in this worker
        start_proxygen_server()
    else:
        logger.info(
            "Running in uWSGI master process (worker_id=0), Proxygen will start in workers"
        )

except (ImportError, AttributeError):
    # Not running under uWSGI or uwsgi module not fully initialized
    logger.warning("Not running under uWSGI - starting Proxygen directly for testing")

    # Register signal handlers
    signal.signal(signal.SIGTERM, signal_handler)
    signal.signal(signal.SIGINT, signal_handler)

    # Start server if running as main module
    if __name__ == "__main__":
        start_proxygen_server()
        try:
            # Keep process alive
            _proxygen_server.wait()
        except KeyboardInterrupt:
            logger.info("Received Ctrl+C, shutting down")
            stop_proxygen_server()


# WSGI application for compatibility (not actually used by Proxygen)
# This is here so uWSGI can load the module without errors
from django.core.wsgi import get_wsgi_application

application = get_wsgi_application()

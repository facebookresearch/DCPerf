#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""
Django server using Proxygen Python binding

This integrates Proxygen with DjangoBench via ASGI.
"""

import logging
import os
import signal
import sys

from django_asgi_adapter import create_django_handler
from proxygen_binding import init_logging, ProxygenServer

logging.basicConfig(
    level=logging.INFO, format="%(asctime)s - %(name)s - %(levelname)s - %(message)s"
)

logger = logging.getLogger(__name__)

# Global reference to server for signal handling
_server = None


def signal_handler(signum, frame):
    """Handle shutdown signals gracefully"""
    logger.info("Received signal %d, shutting down...", signum)
    if _server:
        _server.stop()
    sys.exit(0)


def main() -> None:
    """Main entry point for Django + Proxygen server"""
    global _server

    init_logging()

    django_settings = os.environ.get("DJANGO_SETTINGS_MODULE")
    if not django_settings:
        django_settings = "django_workload.settings"
        os.environ["DJANGO_SETTINGS_MODULE"] = django_settings
        logger.info("Using default Django settings: %s", django_settings)

    ip = os.environ.get("PROXYGEN_IP", "0.0.0.0")
    port = int(os.environ.get("PROXYGEN_PORT", "8000"))
    threads = int(os.environ.get("PROXYGEN_THREADS", "0"))

    if len(sys.argv) > 1:
        port = int(sys.argv[1])
    if len(sys.argv) > 2:
        threads = int(sys.argv[2])

    logger.info("Creating Django ASGI handler (ASYNC MODE)...")
    django_handler = create_django_handler(django_settings)
    logger.info("Django handler created successfully")

    logger.info(
        "Starting Proxygen + Django server (ASYNC MODE) on %s:%d with %d threads",
        ip,
        port,
        threads,
    )
    logger.info("Django settings: %s", django_settings)
    logger.info("NOTE: Using TRUE ASYNC architecture for concurrent request processing")
    logger.info("Press Ctrl+C to stop")

    # Use async constructor - this enables true concurrent request processing
    _server = ProxygenServer(
        ip=ip,
        port=port,
        threads=threads,
        async_callback=django_handler,
        is_async=True,
    )

    # Register signal handlers
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    try:
        _server.start()
        logger.info("Server started successfully")
        logger.info("Test it with: curl http://%s:%d/", ip, port)
        _server.wait()
    except KeyboardInterrupt:
        logger.info("Shutting down server...")
        _server.stop()
        logger.info("Server stopped")
    except Exception as e:
        logger.exception("Error running server: %s", e)
        sys.exit(1)


if __name__ == "__main__":
    main()

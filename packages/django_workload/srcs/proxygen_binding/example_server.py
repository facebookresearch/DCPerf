#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""
Example server using Proxygen Python binding

This demonstrates a basic HTTP server using the Proxygen binding.
"""

import logging
import signal
import sys

from proxygen_binding import init_logging, ProxygenServer, RequestData, ResponseData

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


def handle_request(request: RequestData) -> ResponseData:
    """
    Simple request handler that echoes back request information

    Args:
        request: Proxygen request data

    Returns:
        ResponseData with the response
    """
    logger.debug(
        "Received request: %s %s from %s",
        request.method,
        request.path,
        request.headers.get("Host", "unknown"),
    )

    response = ResponseData()
    response.status_code = 200
    response.status_message = "OK"
    response.headers = {
        "Content-Type": "text/plain",
        "Server": "Proxygen-Python-Binding",
    }

    body_lines = [
        "Proxygen Python Binding - Echo Server\n",
        "=" * 50,
        "\n\n",
        f"Method: {request.method}\n",
        f"Path: {request.path}\n",
        f"URL: {request.url}\n",
        f"Query String: {request.query_string}\n",
        f"HTTP Version: {request.http_version}\n",
        "\nHeaders:\n",
    ]

    for name, value in sorted(request.headers.items()):
        body_lines.append(f"  {name}: {value}\n")

    if request.body:
        body_lines.append(f"\nBody ({len(request.body)} bytes):\n")
        body_lines.append(request.body[:500])
        if len(request.body) > 500:
            body_lines.append(f"\n... ({len(request.body) - 500} more bytes)")

    response.body = "".join(body_lines)
    return response


def main() -> None:
    """Main entry point"""
    global _server

    init_logging()

    ip = "127.0.0.1"
    port = 8000
    threads = 0

    if len(sys.argv) > 1:
        port = int(sys.argv[1])
    if len(sys.argv) > 2:
        threads = int(sys.argv[2])

    logger.info("Starting Proxygen server on %s:%d with %d threads", ip, port, threads)
    logger.info("Press Ctrl+C to stop")

    _server = ProxygenServer(
        ip=ip,
        port=port,
        threads=threads,
        callback=handle_request,
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

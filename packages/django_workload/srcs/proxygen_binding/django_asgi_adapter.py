# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""
Django ASGI adapter for Proxygen binding

This module provides integration between Proxygen's request/response model
and Django's ASGI application interface.
"""

import asyncio
import logging
import threading
import time
from typing import Any, Callable, Dict
from urllib.parse import parse_qs, unquote, urlparse

from proxygen_binding import RequestData, ResponseData

logger = logging.getLogger(__name__)

# Performance debugging support
try:
    from quick_perf_debug import get_request_tracker

    _PERF_DEBUG_ENABLED = True
    logger.info("Performance debugging enabled for ASGI adapter")
except ImportError:
    _PERF_DEBUG_ENABLED = False


def _log_perf(message: str) -> None:
    """Log performance debug message with timing"""
    if logger.isEnabledFor(logging.DEBUG):
        thread_name = threading.current_thread().name
        thread_id = threading.current_thread().ident
        logger.debug(f"[PERF:{time.time():.3f}] [{thread_name}:{thread_id}] {message}")


class ASGIRequestHandler:
    """
    Handles conversion between Proxygen requests and ASGI protocol

    TRUE ASYNC MODE: Returns coroutines that are scheduled on the event loop by C++.
    Multiple requests can be processed concurrently on the same event loop.

    This implementation enables Instagram-style async architecture where coroutines
    are scheduled by C++ and run concurrently without blocking Proxygen threads.
    """

    def __init__(self, asgi_app: Callable) -> None:
        """
        Initialize the handler with a Django ASGI application

        Args:
            asgi_app: Django ASGI application (e.g., from get_asgi_application())
        """
        self.asgi_app = asgi_app

    async def __call__(self, request: RequestData) -> ResponseData:
        """
        Handle a Proxygen request and return a response (ASYNC)

        This is an async function that returns a coroutine. The C++ layer
        schedules this coroutine on the event loop and retrieves the result
        when it completes. This allows multiple requests to be processed
        concurrently without blocking.

        Args:
            request: Proxygen request data

        Returns:
            ResponseData coroutine that resolves when request is complete
        """
        # Return the coroutine directly - C++ will schedule it
        return await self.handle_request_async(request)

    async def handle_request_async(self, request: RequestData) -> ResponseData:
        """
        Async handler for processing requests through ASGI

        Args:
            request: Proxygen request data

        Returns:
            ResponseData for Proxygen to send
        """
        thread_name = threading.current_thread().name
        thread_id = threading.current_thread().ident

        # Performance debugging - track request stages
        request_start_time = time.time()
        _log_perf(f"🚀 Request started: {request.method} {request.path}")

        logger.debug(
            "Starting request: %s %s (thread: %s [%s])",
            request.method,
            request.path,
            thread_name,
            thread_id,
        )

        try:
            scope_start = time.time()
            scope = self.build_asgi_scope(request)
            _log_perf(f"📋 ASGI scope built in {time.time() - scope_start:.3f}s")
        except Exception as e:
            logger.exception("Error building ASGI scope: %s", e)
            return self.create_error_response(500, "Error building ASGI scope")

        response_started = False
        status_code = 200
        status_message = "OK"
        response_headers: Dict[bytes, bytes] = {}
        response_body_parts = []

        # Track whether we've already sent the body
        body_sent = False

        async def receive() -> Dict[str, Any]:
            """
            ASGI receive callable

            Per ASGI spec: First call returns the body, subsequent calls should
            block waiting for disconnect (which never comes in our case).

            IMPORTANT: Django 5.2 calls receive() multiple times during normal
            processing. We must NOT send http.disconnect unless the client actually
            disconnects. Instead, we block indefinitely on subsequent calls.
            """
            nonlocal body_sent

            if not body_sent:
                # First call: return the request body
                body_sent = True
                message = {
                    "type": "http.request",
                    "body": request.body.encode("utf-8") if request.body else b"",
                    "more_body": False,
                }
                logger.debug(
                    f"[ASGI RECEIVE] Sending http.request with {len(message['body'])} bytes"
                )
                return message
            else:
                # Subsequent calls: block indefinitely
                # In a real ASGI server, this would wait for client disconnect
                # In our case, Django will finish sending the response before
                # calling receive() again, so this should never actually be reached
                # during normal operation
                logger.debug(f"[ASGI RECEIVE] Blocking on subsequent receive() call")
                # Block forever - Django should finish sending response before this
                await asyncio.Event().wait()
                # This line is never reached in normal operation
                return {"type": "http.disconnect"}

        async def send(message: Dict[str, Any]) -> None:
            """ASGI send callable"""
            nonlocal response_started, status_code, status_message, response_headers

            logger.debug(f"[ASGI SEND] Received message type: {message['type']}")

            if message["type"] == "http.response.start":
                response_started = True
                status_code = message["status"]
                status_message = self.get_status_message(status_code)

                for header_name, header_value in message.get("headers", []):
                    response_headers[header_name] = header_value

                logger.debug(
                    f"[ASGI SEND] Response started: {status_code} {status_message}"
                )

            elif message["type"] == "http.response.body":
                body = message.get("body", b"")
                more_body = message.get("more_body", False)
                logger.debug(
                    f"[ASGI SEND] Body chunk received: {len(body)} bytes, more_body={more_body}"
                )
                if body:
                    response_body_parts.append(body)

        try:
            asgi_start = time.time()
            _log_perf(f"🌟 Calling Django ASGI app...")
            await self.asgi_app(scope, receive, send)
            asgi_duration = time.time() - asgi_start
            _log_perf(f"✅ Django ASGI app completed in {asgi_duration:.3f}s")
        except Exception as e:
            asgi_duration = time.time() - asgi_start
            _log_perf(f"❌ Django ASGI app failed in {asgi_duration:.3f}s: {e}")
            logger.exception("Exception in ASGI application: %s", e)
            return self.create_error_response(500, "Internal Server Error")

        response = self.build_response(
            status_code, status_message, response_headers, response_body_parts
        )

        logger.debug(
            "Request completed: %s %s -> %d %s",
            request.method,
            request.path,
            response.status_code,
            response.status_message,
        )

        return response

    def build_asgi_scope(self, request: RequestData) -> Dict[str, Any]:
        """
        Build ASGI scope dict from Proxygen request

        Args:
            request: Proxygen request data

        Returns:
            ASGI scope dictionary
        """
        parsed_url = urlparse(request.url or request.path)
        path = unquote(parsed_url.path or "/")
        query_string = request.query_string.encode("latin1")

        headers = []
        for name, value in request.headers.items():
            headers.append((name.lower().encode("latin1"), value.encode("latin1")))

        scope = {
            "type": "http",
            "asgi": {"version": "3.0"},
            "http_version": request.http_version or "1.1",
            "method": request.method or "GET",
            "scheme": "http",
            "path": path,
            "query_string": query_string,
            "root_path": "",
            "headers": headers,
            "server": ("127.0.0.1", 8000),
        }

        return scope

    def build_response(
        self,
        status_code: int,
        status_message: str,
        headers: Dict[bytes, bytes],
        body_parts: list,
    ) -> ResponseData:
        """
        Build Proxygen response from ASGI data

        Args:
            status_code: HTTP status code
            status_message: HTTP status message
            headers: Response headers as bytes dict
            body_parts: List of body byte chunks

        Returns:
            ResponseData for Proxygen
        """
        response = ResponseData()
        response.status_code = status_code
        response.status_message = status_message

        response.headers = {}
        for name_bytes, value_bytes in headers.items():
            name = name_bytes.decode("latin1")
            value = value_bytes.decode("latin1")
            response.headers[name] = value

        if body_parts:
            response.body = b"".join(body_parts).decode("utf-8", errors="replace")

        return response

    def create_error_response(self, status_code: int, message: str) -> ResponseData:
        """
        Create an error response

        Args:
            status_code: HTTP status code
            message: Error message

        Returns:
            ResponseData with error
        """
        response = ResponseData()
        response.status_code = status_code
        response.status_message = message
        response.headers = {"Content-Type": "text/plain"}
        response.body = f"{message}\n"
        return response

    @staticmethod
    def get_status_message(status_code: int) -> str:
        """Get HTTP status message for status code"""
        status_messages = {
            200: "OK",
            201: "Created",
            204: "No Content",
            301: "Moved Permanently",
            302: "Found",
            304: "Not Modified",
            400: "Bad Request",
            401: "Unauthorized",
            403: "Forbidden",
            404: "Not Found",
            405: "Method Not Allowed",
            500: "Internal Server Error",
            502: "Bad Gateway",
            503: "Service Unavailable",
        }
        return status_messages.get(status_code, "Unknown")


def create_django_handler(django_settings_module: str = None) -> Callable:
    """
    Create a Proxygen-compatible handler for Django

    Args:
        django_settings_module: Django settings module path
                                (e.g., 'django_workload.settings')

    Returns:
        Callable that handles Proxygen requests
    """
    import os

    if django_settings_module:
        os.environ.setdefault("DJANGO_SETTINGS_MODULE", django_settings_module)

    try:
        from django.core.asgi import get_asgi_application
    except ImportError as e:
        raise ImportError(
            "Django is not installed. Install it with: pip install django"
        ) from e

    asgi_app = get_asgi_application()
    handler = ASGIRequestHandler(asgi_app)

    return handler

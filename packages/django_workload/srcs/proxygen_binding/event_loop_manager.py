#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""
Event Loop Manager for Proxygen Worker Threads (TRUE ASYNC VERSION)

This module manages asyncio event loops for Proxygen worker threads.
Each thread gets its own long-lived event loop that runs continuously
in a background thread, enabling true async request processing.

Key Features:
- Each Proxygen thread gets a background event loop thread
- Coroutines are scheduled non-blocking via thread-safe calls
- Multiple requests can be processed concurrently per thread
- Responses are sent via callbacks when coroutines complete

This matches Instagram's async architecture where Proxygen threads
are never blocked by Python coroutine execution.
"""

import asyncio
import concurrent.futures
import logging
import queue
import threading
from typing import Any, Callable, Optional

logger = logging.getLogger(__name__)


class BackgroundEventLoop:
    """
    Manages a single event loop running in a background thread.

    This allows the main Proxygen thread to schedule coroutines
    without blocking, enabling true async concurrency.
    """

    def __init__(self, thread_name: str) -> None:
        """
        Initialize background event loop for a thread.

        Args:
            thread_name: Name of the Proxygen thread this loop serves
        """
        self.thread_name = thread_name
        self.loop: Optional[asyncio.AbstractEventLoop] = None
        self.loop_thread: Optional[threading.Thread] = None
        self._stop_event = threading.Event()
        self._started_event = threading.Event()
        self._shutdown = False

    def start(self) -> None:
        """Start the background event loop thread"""
        if self.loop_thread and self.loop_thread.is_alive():
            return  # Already started

        logger.debug(
            "[ASYNC_LOOP] Starting background event loop for thread %s",
            self.thread_name,
        )

        self.loop_thread = threading.Thread(
            target=self._run_event_loop,
            name=f"AsyncLoop-{self.thread_name}",
            daemon=True,  # Dies when main thread dies
        )
        self.loop_thread.start()

        # Wait for event loop to be ready
        self._started_event.wait(timeout=5.0)

        if not self._started_event.is_set():
            raise RuntimeError(f"Failed to start event loop for {self.thread_name}")

        logger.debug(
            "[ASYNC_LOOP] Background event loop ready for thread %s",
            self.thread_name,
        )

    def _run_event_loop(self) -> None:
        """Run the event loop in the background thread (INTERNAL)"""
        try:
            # Create new event loop for this background thread
            self.loop = asyncio.new_event_loop()
            asyncio.set_event_loop(self.loop)

            logger.debug(
                "[ASYNC_LOOP] Event loop created in background thread for %s",
                self.thread_name,
            )

            # Signal that we're ready to accept coroutines
            self._started_event.set()

            # Run event loop forever until stopped
            self.loop.run_forever()

        except Exception as e:
            logger.error(
                "[ASYNC_LOOP] Event loop crashed for thread %s: %s",
                self.thread_name,
                e,
            )
        finally:
            # Clean up
            if self.loop and not self.loop.is_closed():
                logger.debug(
                    "[ASYNC_LOOP] Closing event loop for thread %s",
                    self.thread_name,
                )
                self.loop.close()

    def schedule_coroutine(
        self, coro: Any, done_callback: Callable[[Any], None]
    ) -> None:
        """
        Schedule a coroutine on the background event loop (NON-BLOCKING).

        This is the key function that enables true async - it schedules
        the coroutine and returns immediately without waiting.

        Args:
            coro: Coroutine to schedule
            done_callback: Callback to call when coroutine completes
        """
        if self._shutdown or not self.loop or self.loop.is_closed():
            logger.error(
                "[ASYNC_LOOP] Cannot schedule coroutine: loop not running for %s",
                self.thread_name,
            )
            return

        try:
            # Schedule coroutine on the background event loop (thread-safe)
            future = asyncio.run_coroutine_threadsafe(coro, self.loop)

            # Add callback to be called when coroutine completes
            # This callback runs in the background thread
            future.add_done_callback(done_callback)

            logger.debug(
                "[ASYNC_LOOP] Coroutine scheduled for thread %s",
                self.thread_name,
            )

        except Exception as e:
            logger.error(
                "[ASYNC_LOOP] Failed to schedule coroutine for %s: %s",
                self.thread_name,
                e,
            )

    def shutdown(self) -> None:
        """Shutdown the background event loop and thread"""
        if self._shutdown:
            return

        self._shutdown = True

        logger.debug(
            "[ASYNC_LOOP] Shutting down event loop for thread %s",
            self.thread_name,
        )

        # Stop the event loop
        if self.loop and not self.loop.is_closed():
            self.loop.call_soon_threadsafe(self.loop.stop)

        # Wait for background thread to finish
        if self.loop_thread and self.loop_thread.is_alive():
            self.loop_thread.join(timeout=2.0)
            if self.loop_thread.is_alive():
                logger.warning(
                    "[ASYNC_LOOP] Background thread did not shutdown cleanly for %s",
                    self.thread_name,
                )


class EventLoopManager:
    """
    Manages background event loops for Proxygen worker threads using a thread pool.

    Instead of creating one background thread per Proxygen thread, we maintain
    a fixed pool of background event loop threads. Proxygen threads are mapped
    to pool threads using round-robin assignment.

    This prevents thread exhaustion under high concurrency.
    """

    def __init__(self, pool_size: int = 8) -> None:
        """
        Initialize the event loop manager with a thread pool.

        Args:
            pool_size: Number of background event loop threads to create.
                      Default is 8, which should handle most workloads.
        """
        self._thread_local = threading.local()
        self._lock = threading.Lock()
        self._pool_size = pool_size
        self._event_loop_pool: list[BackgroundEventLoop] = []
        self._pool_initialized = False
        self._next_pool_index = 0

        logger.info("[EVENT_LOOP_MANAGER] Initialized with pool size %d", pool_size)

    def _initialize_pool(self) -> None:
        """Initialize the pool of background event loops (called once)"""
        if self._pool_initialized:
            return

        logger.info(
            "[EVENT_LOOP_MANAGER] Creating pool of %d background event loops",
            self._pool_size,
        )

        for i in range(self._pool_size):
            pool_thread_name = f"AsyncPool-{i}"
            background_loop = BackgroundEventLoop(pool_thread_name)

            try:
                background_loop.start()
                self._event_loop_pool.append(background_loop)
                logger.info(
                    "[EVENT_LOOP_MANAGER] Pool thread %d/%d started: %s",
                    i + 1,
                    self._pool_size,
                    pool_thread_name,
                )
            except RuntimeError as e:
                logger.error(
                    "[EVENT_LOOP_MANAGER] Failed to start pool thread %d: %s", i, e
                )
                # Continue with smaller pool
                break

        if not self._event_loop_pool:
            raise RuntimeError("Failed to create any background event loop threads")

        self._pool_initialized = True
        logger.info(
            "[EVENT_LOOP_MANAGER] Pool initialized with %d threads",
            len(self._event_loop_pool),
        )

    def get_or_create_loop(self) -> BackgroundEventLoop:
        """
        Get background event loop for current thread.

        Maps the current thread to one of the pooled background event loops
        using round-robin assignment. Multiple Proxygen threads may share
        the same background event loop.

        Returns:
            BackgroundEventLoop from the pool
        """
        # Check if this thread already has a background event loop assigned
        if not hasattr(self._thread_local, "background_loop"):
            with self._lock:
                # Double-check after acquiring lock
                if not hasattr(self._thread_local, "background_loop"):
                    # Initialize pool if needed (first call)
                    if not self._pool_initialized:
                        self._initialize_pool()

                    # Assign a background loop from the pool (round-robin)
                    self._assign_loop_from_pool()

        return self._thread_local.background_loop

    def _assign_loop_from_pool(self) -> None:
        """Assign a background event loop from the pool to current thread"""
        thread_id = threading.current_thread().ident
        thread_name = threading.current_thread().name

        # Round-robin assignment
        pool_index = self._next_pool_index % len(self._event_loop_pool)
        background_loop = self._event_loop_pool[pool_index]
        self._next_pool_index += 1

        # Store in thread-local storage
        self._thread_local.background_loop = background_loop

        logger.debug(
            "[EVENT_LOOP_MANAGER] Assigned pool thread '%s' to Proxygen thread %s [%s]",
            background_loop.thread_name,
            thread_name,
            thread_id,
        )

    def close_current_loop(self) -> None:
        """
        Close background event loop for current thread.

        This should be called when a Proxygen thread is shutting down.
        """
        if hasattr(self._thread_local, "background_loop"):
            background_loop = self._thread_local.background_loop
            thread_name = threading.current_thread().name

            logger.debug(
                "[EVENT_LOOP] Closing background event loop for thread %s",
                thread_name,
            )

            # Shutdown the background loop
            try:
                background_loop.shutdown()
                logger.debug(
                    "[EVENT_LOOP] Background event loop closed for thread %s",
                    thread_name,
                )
            except Exception as e:
                logger.error(
                    "[EVENT_LOOP] Error closing background event loop for thread %s: %s",
                    thread_name,
                    e,
                )

            # Remove from thread-local storage
            delattr(self._thread_local, "background_loop")


# Global event loop manager instance
_event_loop_manager: Optional[EventLoopManager] = None
_manager_lock = threading.Lock()


def get_event_loop_manager() -> EventLoopManager:
    """
    Get the global event loop manager instance.

    Returns:
        Global EventLoopManager instance
    """
    global _event_loop_manager

    if _event_loop_manager is None:
        with _manager_lock:
            if _event_loop_manager is None:
                _event_loop_manager = EventLoopManager()
                logger.debug("[EVENT_LOOP] Global EventLoopManager initialized")

    return _event_loop_manager


def initialize_thread_event_loop() -> asyncio.AbstractEventLoop:
    """
    Initialize event loop for current thread.

    This should be called once per thread (typically when thread starts).

    Returns:
        Event loop for this thread
    """
    manager = get_event_loop_manager()
    return manager.get_or_create_loop()


def get_current_loop() -> asyncio.AbstractEventLoop:
    """
    Get event loop for current thread.

    Raises:
        RuntimeError: If event loop hasn't been initialized for this thread

    Returns:
        Event loop for this thread
    """
    try:
        loop = asyncio.get_event_loop()
        if loop.is_closed():
            raise RuntimeError("Event loop is closed")
        return loop
    except RuntimeError:
        # No loop exists, create one
        return initialize_thread_event_loop()


def cleanup_thread_event_loop() -> None:
    """
    Clean up event loop for current thread.

    This should be called when thread is shutting down.
    """
    manager = get_event_loop_manager()
    manager.close_current_loop()

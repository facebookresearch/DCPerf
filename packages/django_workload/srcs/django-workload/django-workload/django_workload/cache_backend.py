# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Thread-safe, fault-tolerant memcached cache backend for Django using pylibmc.

pylibmc.Client is NOT thread-safe — concurrent access from multiple threads
corrupts the memcached protocol stream (interleaved request/response bytes),
causing ~50% ConnectionError/SocketCreateError/Failure rates.

Django's CacheHandler creates per-thread cache backend instances, so
instance-level locks don't provide cross-thread protection. This backend
uses module-level (global) state: one lock and one pylibmc.Client per
process, shared across all FaultTolerantPyLibMCCache instances. This
ensures only one thread accesses memcached at a time while keeping
memory usage minimal (one client per process).

Errors are swallowed and treated as cache misses to prevent cascading
failures in DjangoBench's multi-threaded Proxygen architecture.
"""

from __future__ import annotations

import logging
import os
import re
import threading
import time

import pylibmc
from django.core.cache.backends.base import BaseCache, DEFAULT_TIMEOUT

logger = logging.getLogger(__name__)

# How often (seconds) each worker logs its cache stats summary
_STATS_LOG_INTERVAL = 30

# Module-level globals shared across ALL cache backend instances in a process.
# Django's CacheHandler creates per-thread instances, but we need ONE lock
# and ONE client per process to prevent pylibmc protocol corruption.
_global_lock = threading.Lock()
_global_client = None
_global_client_pid = None


def _get_client(servers, options):
    """Get or create the process-wide pylibmc client.

    Must be called while holding _global_lock.
    """
    global _global_client, _global_client_pid
    pid = os.getpid()
    if _global_client_pid != pid:
        _global_client = pylibmc.Client(servers, **options)
        _global_client_pid = pid
    return _global_client


class _CacheStats:
    """Per-process cache operation counters with periodic logging."""

    def __init__(self):
        self._lock = threading.Lock()
        self._pid = os.getpid()
        self._reset_counters()

    def _reset_counters(self):
        self._ops = 0
        self._errors = 0
        self._error_types = {}  # exception class name -> count
        self._last_log = time.monotonic()

    def record_success(self):
        with self._lock:
            self._maybe_reset_pid()
            self._ops += 1
            self._maybe_log()

    def record_error(self, exc):
        with self._lock:
            self._maybe_reset_pid()
            self._ops += 1
            self._errors += 1
            name = type(exc).__name__
            self._error_types[name] = self._error_types.get(name, 0) + 1
            self._maybe_log()

    def _maybe_reset_pid(self):
        pid = os.getpid()
        if self._pid != pid:
            self._pid = pid
            self._reset_counters()

    def _maybe_log(self):
        now = time.monotonic()
        if now - self._last_log >= _STATS_LOG_INTERVAL:
            elapsed = now - self._last_log
            ops, errors = self._ops, self._errors
            error_types = dict(self._error_types)
            self._ops = 0
            self._errors = 0
            self._error_types = {}
            self._last_log = now
            rate = ops / elapsed if elapsed > 0 else 0
            if errors > 0:
                logger.warning(
                    "cache stats [pid=%d] %.0fs: %d ops (%.1f/s), "
                    "%d errors (%.1f%%) — %s",
                    self._pid,
                    elapsed,
                    ops,
                    rate,
                    errors,
                    100.0 * errors / ops if ops else 0,
                    ", ".join(f"{k}={v}" for k, v in error_types.items()),
                )
            else:
                logger.info(
                    "cache stats [pid=%d] %.0fs: %d ops (%.1f/s), 0 errors",
                    self._pid,
                    elapsed,
                    ops,
                    rate,
                )


_stats = _CacheStats()


class FaultTolerantPyLibMCCache(BaseCache):
    """pylibmc backend that swallows errors and treats them as cache misses.

    Uses module-level lock and client shared across all instances in a
    process. Django creates per-thread instances, but we bypass that by
    using global state, ensuring one pylibmc.Client per process.
    """

    def __init__(self, server, params):
        super().__init__(params)
        if isinstance(server, str):
            self._servers = [s.removeprefix("unix:") for s in re.split("[;,]", server)]
        else:
            self._servers = [s.removeprefix("unix:") for s in server]
        self._options = params.get("OPTIONS") or {}

    @property
    def _cache(self):
        return _get_client(self._servers, self._options)

    def get_backend_timeout(self, timeout=DEFAULT_TIMEOUT):
        if timeout == DEFAULT_TIMEOUT:
            timeout = self.default_timeout
        if timeout is None:
            return 0
        elif int(timeout) == 0:
            timeout = -1
        if timeout > 2592000:
            import time

            timeout += int(time.time())
        return int(timeout)

    def make_and_validate_key(self, key, version=None):
        key = self.make_key(key, version=version)
        self.validate_key(key)
        return key

    def add(self, key, value, timeout=DEFAULT_TIMEOUT, version=None):
        key = self.make_and_validate_key(key, version=version)
        try:
            with _global_lock:
                result = self._cache.add(key, value, self.get_backend_timeout(timeout))
            _stats.record_success()
            return result
        except Exception as e:
            _stats.record_error(e)
            return False

    def get(self, key, default=None, version=None):
        key = self.make_and_validate_key(key, version=version)
        try:
            with _global_lock:
                val = self._cache.get(key)
            _stats.record_success()
        except Exception as e:
            _stats.record_error(e)
            val = None
        return val if val is not None else default

    def set(self, key, value, timeout=DEFAULT_TIMEOUT, version=None):
        key = self.make_and_validate_key(key, version=version)
        try:
            with _global_lock:
                if not self._cache.set(key, value, self.get_backend_timeout(timeout)):
                    self._cache.delete(key)
            _stats.record_success()
        except Exception as e:
            _stats.record_error(e)

    def touch(self, key, timeout=DEFAULT_TIMEOUT, version=None):
        key = self.make_and_validate_key(key, version=version)
        try:
            with _global_lock:
                if timeout == 0:
                    result = self._cache.delete(key)
                else:
                    result = self._cache.touch(key, self.get_backend_timeout(timeout))
            _stats.record_success()
            return result
        except Exception as e:
            _stats.record_error(e)
            return False

    def delete(self, key, version=None):
        key = self.make_and_validate_key(key, version=version)
        try:
            with _global_lock:
                result = bool(self._cache.delete(key))
            _stats.record_success()
            return result
        except Exception as e:
            _stats.record_error(e)
            return False

    def get_many(self, keys, version=None):
        key_map = {
            self.make_and_validate_key(key, version=version): key for key in keys
        }
        try:
            with _global_lock:
                ret = self._cache.get_multi(key_map.keys())
            _stats.record_success()
            return {key_map[k]: v for k, v in ret.items()}
        except Exception as e:
            _stats.record_error(e)
            return {}

    def set_many(self, data, timeout=DEFAULT_TIMEOUT, version=None):
        safe_data = {}
        original_keys = {}
        for key, value in data.items():
            safe_key = self.make_and_validate_key(key, version=version)
            safe_data[safe_key] = value
            original_keys[safe_key] = key
        try:
            with _global_lock:
                failed_keys = self._cache.set_multi(
                    safe_data, self.get_backend_timeout(timeout)
                )
            _stats.record_success()
            return [original_keys[k] for k in failed_keys]
        except Exception as e:
            _stats.record_error(e)
            return list(data.keys())

    def delete_many(self, keys, version=None):
        keys = [self.make_and_validate_key(key, version=version) for key in keys]
        try:
            with _global_lock:
                self._cache.delete_multi(keys)
            _stats.record_success()
        except Exception as e:
            _stats.record_error(e)

    def incr(self, key, delta=1, version=None):
        key = self.make_and_validate_key(key, version=version)
        try:
            with _global_lock:
                if delta < 0:
                    val = self._cache.decr(key, -delta)
                else:
                    val = self._cache.incr(key, delta)
        except Exception:
            val = None
        if val is None:
            raise ValueError("Key '%s' not found" % key)
        return val

    def clear(self):
        try:
            with _global_lock:
                self._cache.flush_all()
        except Exception:
            pass

    def close(self, **kwargs):
        pass

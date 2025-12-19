# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
Inbox endpoint handler for DjangoBench V2.

This module models the workload characteristics of activity.api.views.inbox
from production IG Django server.

The inbox endpoint is a data aggregation endpoint for Instagram Direct (IGD)
that provides the client with a snapshot of all thread and message metadata
needed to load and present the inbox UI.

Key features modeled:
- Thread and message data aggregation from multiple sources
- User metadata fetching via NodeAPI/LazyUserDict patterns
- Spam filtering via microservice calls
- Real-time updates via Iris subscriptions
- Read state management and badge calculations
- Caching with Direct cache patterns
- CPU-intensive primitives based on production workload profiles
"""

import logging
import time
from typing import Any, Dict, Optional

from django.core.cache import cache

from .inbox import execute_inbox_random_primitives, InboxResponse, InboxService


logger = logging.getLogger(__name__)


class Inbox:
    """
    Handler for inbox endpoint.

    Models the workload of activity.api.views.inbox from production IG.
    Uses InboxService for full production-like behavior with:
    - Thread/message aggregation via Thrift RPC
    - Spam filtering via microservice calls
    - User metadata fetching via NodeAPI patterns
    - Read state management and badge calculations
    - Caching with Direct cache patterns
    - CPU-intensive primitives
    """

    NUM_CPU_PRIMITIVES = 1
    CACHE_TTL = 30

    def __init__(self, request):
        self.request = request
        self.user = request.user
        self._inbox_service: Optional[InboxService] = None

    def _get_inbox_service(self) -> InboxService:
        """Lazily initialize InboxService."""
        if self._inbox_service is None:
            self._inbox_service = InboxService(self.request, self.user)
        return self._inbox_service

    def results(self) -> Dict[str, Any]:
        """
        Main entry point for inbox data.

        Returns:
            Dict with inbox threads and metadata
        """
        start_time = time.time()

        user = self.request.user
        key = "inbox.{}".format(user.id.hex)
        cached = cache.get(key)
        if cached is not None:
            return cached

        primitive_results = execute_inbox_random_primitives(
            num_executions=self.NUM_CPU_PRIMITIVES
        )
        logger.debug(
            f"[perf] inbox::cpu_primitives: {time.time() - start_time:.4f}s, "
            f"executed={len(primitive_results)}"
        )

        service = self._get_inbox_service()
        response: InboxResponse = service.get_inbox()

        result = response.to_dict()
        cache.set(key, result, self.CACHE_TTL)

        logger.debug(
            f"[perf] inbox::results: {time.time() - start_time:.4f}s, "
            f"threads={len(response.threads)}, badge={response.badge_count}"
        )

        return result

    def post_process(self, result: Dict[str, Any]) -> Dict[str, Any]:
        """
        Post-process inbox results.

        Applies deduplication and statistics computation for CPU-intensive work.

        Args:
            result: Raw inbox result dict

        Returns:
            Processed result dict with summary
        """
        threads = result.get("threads", [])
        config = InboxV2Config()

        for _ in range(config.mult_factor):
            config.list_extend(threads)

        sorted_threads = sorted(
            config.get_list(),
            key=lambda x: x.get("last_activity_at", 0),
            reverse=True,
        )

        final_threads = []
        seen_ids = set()
        for thread in sorted_threads:
            tid = thread.get("thread_id")
            if tid not in seen_ids:
                seen_ids.add(tid)
                final_threads.append(thread)
                config.total_unread += thread.get("unread_count", 0)

        result["threads"] = final_threads
        result["processed_total_unread"] = config.total_unread
        return result


class InboxV2Config:
    """Configuration for V2 inbox processing."""

    def __init__(self):
        self.mult_factor = 1
        self.work_list = []
        self.total_unread = 0

    def list_extend(self, list_):
        self.work_list.extend(list_)

    def get_list(self):
        return self.work_list

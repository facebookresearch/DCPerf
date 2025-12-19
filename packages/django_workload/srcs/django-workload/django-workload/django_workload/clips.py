# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
Clips endpoint for DjangoBench V2.

This module implements the clips endpoint that models the workload of
clips.api.views.async_stream_clips_discover from production IG Django server.

The endpoint serves Reels/Clips discovery with:
- Organic clips from Cassandra database
- Ads blended in via Thrift RPC
- Caching via memcached
- Optional request parameters for pagination and filtering
"""

import logging

from .clips_discovery import ClipsDiscoverService, ClipsDiscoverStreamingService

logger = logging.getLogger(__name__)


class Clips:
    """
    Clips endpoint handler.

    Models clips.api.views.async_stream_clips_discover from production IG Django.
    """

    def __init__(self, request):
        """Initialize clips handler with request."""
        self.request = request
        self.user = request.user

    def discover(self):
        """
        Main clips discovery endpoint.

        Returns:
            dict with clips discovery response
        """
        service = ClipsDiscoverService(self.request, self.user)
        response = service.discover()
        return response.to_dict()

    def stream_discover(self):
        """
        Streaming clips discovery endpoint.

        Yields:
            Generator of dict responses for chunked streaming
        """
        service = ClipsDiscoverStreamingService(self.request, self.user)
        for chunk in service.stream_discover():
            yield chunk.to_dict()

    def post_process(self, result):
        """
        Post-process clips results.

        Adds additional CPU work to match production workload patterns.

        Args:
            result: Clips discovery response dict

        Returns:
            Processed result dict
        """
        config = ClipsConfig()
        items = result.get("items_with_ads", [])

        # Apply multiplication factor for CPU work
        for _ in range(config.mult_factor):
            config.list_extend(items)

        # Sort by quality score
        sorted_items = sorted(
            config.get_list(),
            key=lambda x: x.get("quality_score", 0),
            reverse=True,
        )

        # Deduplicate
        final_items = []
        seen_pks = set()
        for item in sorted_items:
            pk = item.get("pk")
            if pk not in seen_pks:
                seen_pks.add(pk)
                final_items.append(item)

                # Track metrics
                config.total_views += item.get("view_count", 0)
                config.views_per_clip[pk] = item.get("view_count", 0)

        result["items_with_ads"] = final_items
        result["total_views"] = config.total_views
        return result


class ClipsConfig:
    """Configuration and state for clips post-processing."""

    def __init__(self):
        self.mult_factor = 1
        self.work_list = []
        self.total_views = 0
        self.views_per_clip = {}

    def list_extend(self, list_):
        self.work_list.extend(list_)

    def get_list(self):
        return self.work_list

# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
Seen endpoint handler for DjangoBench V2.

This module implements the logic for marking entities as seen.
Supports marking bundles, inbox threads, clips, and feed entries as seen.

The handler supports two modes:
1. Default mode (no parameters): Execute original random-sample logic
2. Targeted mode (with type and id): Mark a specific entity as seen
"""

import json
import logging
import random
import uuid
from typing import Any, Dict, Optional, Tuple

from cassandra.cqlengine.query import BatchQuery
from django.conf import settings
from django.core.cache import cache
from django_statsd.clients import statsd

from .models import (
    BundleEntryModel,
    BundleSeenModel,
    ClipSeenModel,
    ClipVideoModel,
    FeedEntryModel,
    FeedSeenModel,
    InboxReadStateModel,
    InboxThreadModel,
)

logger = logging.getLogger(__name__)


# Entity type constants
ENTITY_TYPE_BUNDLE = "bundle"
ENTITY_TYPE_INBOX = "inbox"
ENTITY_TYPE_CLIP = "clip"
ENTITY_TYPE_FEED_TIMELINE = "feed_timeline"

VALID_ENTITY_TYPES = {
    ENTITY_TYPE_BUNDLE,
    ENTITY_TYPE_INBOX,
    ENTITY_TYPE_CLIP,
    ENTITY_TYPE_FEED_TIMELINE,
}


class SeenHandler:
    """
    Handler for the /seen endpoint.

    Supports marking entities as seen either in batch (default mode)
    or individually (targeted mode with type and id parameters).
    """

    # For sample-based profiling
    _sample_count = 0

    def __init__(self, request):
        """
        Initialize the seen handler.

        Args:
            request: Django HTTP request object
        """
        self.request = request
        self.user = request.user

    def handle(self) -> Tuple[Dict[str, Any], int]:
        """
        Main entry point for the seen endpoint.

        Returns:
            Tuple of (response_dict, http_status_code)
        """
        # Extract parameters from GET or POST data
        entity_type = self._get_param("type")
        entity_id = self._get_param("id")

        # If both parameters are provided, use targeted mode
        if entity_type and entity_id:
            return self._handle_targeted_seen(entity_type, entity_id)

        # Default mode: execute original random-sample logic
        return self._handle_default_seen()

    def _get_param(self, param_name: str) -> Optional[str]:
        """
        Get a parameter from either GET or POST data.

        Args:
            param_name: Name of the parameter to retrieve

        Returns:
            Parameter value or None if not found
        """
        # Check GET parameters first
        value = self.request.GET.get(param_name)
        if value:
            return value

        # Check POST parameters
        value = self.request.POST.get(param_name)
        if value:
            return value

        # Check JSON body for POST requests
        if (
            self.request.method == "POST"
            and self.request.content_type == "application/json"
        ):
            try:
                body = json.loads(self.request.body.decode("utf-8"))
                return body.get(param_name)
            except (json.JSONDecodeError, UnicodeDecodeError):
                pass

        return None

    def _handle_targeted_seen(
        self, entity_type: str, entity_id: str
    ) -> Tuple[Dict[str, Any], int]:
        """
        Handle marking a specific entity as seen.

        Args:
            entity_type: Type of entity (bundle, inbox, clip, feed_timeline)
            entity_id: UUID of the entity to mark as seen (or thread_id for inbox)

        Returns:
            Tuple of (response_dict, http_status_code)
        """
        # Validate entity type
        if entity_type not in VALID_ENTITY_TYPES:
            return {
                "error": f"Invalid entity type: {entity_type}",
                "valid_types": list(VALID_ENTITY_TYPES),
            }, 400

        # For inbox, thread_id is not a UUID (format: thread_xxx_xxx_xxx)
        # Skip UUID validation for inbox entity type
        if entity_type == ENTITY_TYPE_INBOX:
            # Validate thread_id format (should start with "thread_")
            if not entity_id.startswith("thread_"):
                return {
                    "error": f"Invalid inbox thread ID format: {entity_id}",
                    "expected_format": "thread_<timestamp>_<id1>_<id2> (e.g., thread_1762127923031627977_15_8811)",
                }, 400
            return self._mark_inbox_seen_by_thread_id(entity_id)

        # For other entity types, validate UUID format
        try:
            entity_uuid = uuid.UUID(entity_id)
        except ValueError:
            return {
                "error": f"Invalid entity ID format: {entity_id}",
                "expected_format": "UUID (e.g., 550e8400-e29b-41d4-a716-446655440000)",
            }, 400

        # Route to appropriate handler
        if entity_type == ENTITY_TYPE_BUNDLE:
            return self._mark_bundle_seen(entity_uuid)
        elif entity_type == ENTITY_TYPE_CLIP:
            return self._mark_clip_seen(entity_uuid)
        elif entity_type == ENTITY_TYPE_FEED_TIMELINE:
            return self._mark_feed_seen(entity_uuid)

        # Should not reach here due to validation above
        return {"error": "Unknown entity type"}, 500

    def _mark_bundle_seen(self, bundle_id: uuid.UUID) -> Tuple[Dict[str, Any], int]:
        """
        Mark a bundle as seen.

        Args:
            bundle_id: UUID of the bundle to mark as seen

        Returns:
            Tuple of (response_dict, http_status_code)
        """
        # Check if bundle exists
        try:
            bundles = list(BundleEntryModel.objects.filter(id=bundle_id).limit(1))
            if not bundles:
                return {
                    "success": False,
                    "error": f"Bundle not found: {bundle_id}",
                    "type": ENTITY_TYPE_BUNDLE,
                }, 200
        except Exception as e:
            logger.debug(f"Error checking bundle existence: {e}")
            # Continue anyway - bundle might exist in a different partition

        # Create seen record
        try:
            BundleSeenModel(
                userid=self.user.id,
                bundleid=bundle_id,
                entryid=uuid.uuid4(),  # Placeholder entry ID
            ).save()

            logger.debug(f"Marked bundle {bundle_id} as seen for user {self.user.id}")

            return {
                "success": True,
                "type": ENTITY_TYPE_BUNDLE,
                "id": str(bundle_id),
            }, 200
        except Exception as e:
            logger.error(f"Error marking bundle as seen: {e}")
            return {
                "success": False,
                "error": f"Failed to mark bundle as seen: {e}",
            }, 500

    def _mark_inbox_seen(self, thread_id: uuid.UUID) -> Tuple[Dict[str, Any], int]:
        """
        Mark an inbox thread as seen/read.

        Args:
            thread_id: UUID of the inbox thread to mark as seen

        Returns:
            Tuple of (response_dict, http_status_code)
        """
        # Check if thread exists
        try:
            threads = list(
                InboxThreadModel.objects.filter(thread_id=thread_id).limit(1)
            )
            if not threads:
                return {
                    "success": False,
                    "error": f"Inbox thread not found: {thread_id}",
                    "type": ENTITY_TYPE_INBOX,
                }, 200
        except Exception as e:
            logger.warning(f"Error checking thread existence: {e}")

        # Create/update read state record
        try:
            InboxReadStateModel(
                user_id=self.user.id,
                thread_id=thread_id,
                unread_count=0,
            ).save()

            logger.debug(
                f"Marked inbox thread {thread_id} as seen for user {self.user.id}"
            )

            return {
                "success": True,
                "type": ENTITY_TYPE_INBOX,
                "id": str(thread_id),
            }, 200
        except Exception as e:
            logger.error(f"Error marking inbox as seen: {e}")
            return {
                "success": False,
                "error": f"Failed to mark inbox as seen: {e}",
            }, 500

    def _mark_inbox_seen_by_thread_id(
        self, thread_id: str
    ) -> Tuple[Dict[str, Any], int]:
        """
        Mark an inbox thread as seen/read using thread_id string format.

        This method handles thread_id in the format: thread_<timestamp>_<id1>_<id2>
        (e.g., thread_1762127923031627977_15_8811)

        Note: The thread existence check is skipped because the Thrift mock server
        generates thread_ids in a different format (string) than the database model
        expects (UUID). The /seen endpoint is primarily for benchmarking purposes,
        so we accept the thread_id and record the seen state without strict validation.

        Args:
            thread_id: String thread ID in the format thread_xxx_xxx_xxx

        Returns:
            Tuple of (response_dict, http_status_code)
        """
        # Note: We skip thread existence check here because:
        # - The Thrift mock server returns thread_ids in string format (thread_xxx_xxx_xxx)
        # - The InboxThreadModel.thread_id is a UUID column
        # - These formats are incompatible for direct database lookup
        # For benchmarking purposes, we accept the thread_id as-is

        # Create/update read state record
        try:
            # Use user_id as the primary key, thread_id as string identifier
            InboxReadStateModel(
                user_id=self.user.id,
                thread_id=thread_id,
                unread_count=0,
            ).save()

            logger.debug(
                f"Marked inbox thread {thread_id} as seen for user {self.user.id}"
            )

            return {
                "success": True,
                "type": ENTITY_TYPE_INBOX,
                "id": thread_id,
            }, 200
        except Exception as e:
            logger.error(f"Error marking inbox as seen: {e}")
            return {
                "success": False,
                "error": f"Failed to mark inbox as seen: {e}",
            }, 500

    def _mark_clip_seen(self, video_id: uuid.UUID) -> Tuple[Dict[str, Any], int]:
        """
        Mark a clip/video as seen.

        Args:
            video_id: UUID of the clip to mark as seen

        Returns:
            Tuple of (response_dict, http_status_code)
        """
        # Check if clip exists
        try:
            clips = list(ClipVideoModel.objects.filter(id=video_id).limit(1))
            if not clips:
                return {
                    "success": False,
                    "error": f"Clip not found: {video_id}",
                    "type": ENTITY_TYPE_CLIP,
                }, 200
        except Exception as e:
            logger.warning(f"Error checking clip existence: {e}")

        # Create seen record
        try:
            ClipSeenModel(
                userid=self.user.id,
                video_id=video_id,
                watch_duration_ms=0,
                completed=False,
            ).save()

            logger.debug(f"Marked clip {video_id} as seen for user {self.user.id}")

            return {
                "success": True,
                "type": ENTITY_TYPE_CLIP,
                "id": str(video_id),
            }, 200
        except Exception as e:
            logger.error(f"Error marking clip as seen: {e}")
            return {"success": False, "error": f"Failed to mark clip as seen: {e}"}, 500

    def _mark_feed_seen(self, entry_id: uuid.UUID) -> Tuple[Dict[str, Any], int]:
        """
        Mark a feed entry as seen.

        Args:
            entry_id: UUID of the feed entry to mark as seen

        Returns:
            Tuple of (response_dict, http_status_code)
        """
        # Check if feed entry exists
        try:
            entries = list(
                FeedEntryModel.objects.filter(id=entry_id).limit(1).allow_filtering()
            )
            if not entries:
                return {
                    "success": False,
                    "error": f"Feed entry not found: {entry_id}",
                    "type": ENTITY_TYPE_FEED_TIMELINE,
                }, 200
        except Exception as e:
            logger.warning(f"Error checking feed entry existence: {e}")

        # Create seen record
        try:
            FeedSeenModel(
                userid=self.user.id,
                entryid=entry_id,
            ).save()

            logger.debug(
                f"Marked feed entry {entry_id} as seen for user {self.user.id}"
            )

            return {
                "success": True,
                "type": ENTITY_TYPE_FEED_TIMELINE,
                "id": str(entry_id),
            }, 200
        except Exception as e:
            logger.error(f"Error marking feed entry as seen: {e}")
            return {
                "success": False,
                "error": f"Failed to mark feed entry as seen: {e}",
            }, 500

    def _handle_default_seen(self) -> Tuple[Dict[str, Any], int]:
        """
        Handle the default seen behavior (original random-sample logic).

        Records stats for items marked as seen on a mobile device.
        For workload purposes, generates random data cached in memcached.

        Returns:
            Tuple of (response_dict, http_status_code)
        """
        should_profile = False

        if settings.PROFILING:
            SeenHandler._sample_count += 1
            if SeenHandler._sample_count >= settings.SAMPLE_RATE:
                SeenHandler._sample_count = 0
                should_profile = True

        # Get or generate cached bundle IDs
        bundleids = cache.get("bundleids")
        if bundleids is None:
            bundleids = [uuid.uuid4() for _ in range(1000)]
            cache.set("bundleids", bundleids, 24 * 60 * 60)

        # Get or generate cached entry IDs
        entryids = cache.get("entryids")
        if entryids is None:
            entryids = [uuid.uuid4() for _ in range(10000)]
            cache.set("entryids", entryids, 24 * 60 * 60)

        # Batch process random samples
        with statsd.pipeline() as pipe, BatchQuery():
            for bundleid in random.sample(bundleids, random.randrange(3)):
                if should_profile:
                    pipe.incr(f"workloadoutput.bundle.{bundleid.hex}.seen")
                for entryid in random.sample(entryids, random.randrange(5)):
                    if should_profile:
                        pipe.incr(
                            f"workloadoutput.bundle.{bundleid.hex}.{entryid.hex}.seen"
                        )
                    BundleSeenModel(
                        userid=self.request.user.id,
                        bundleid=bundleid,
                        entryid=entryid,
                    ).save()

        return {}, 200

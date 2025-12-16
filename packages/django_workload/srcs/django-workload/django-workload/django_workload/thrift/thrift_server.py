#!/usr/bin/env python3
# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
Thrift RPC Server for MockAdsService.

This server provides real Thrift RPC endpoints that create Python↔Thrift
boundary crossings to generate I-cache misses matching production patterns.

Supports both IPv4 and IPv6 connections via dual-stack socket.
"""

import os
import random
import socket
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

# Disable stdout/stderr buffering for immediate log output
sys.stdout = os.fdopen(sys.stdout.fileno(), "w", buffering=1)
sys.stderr = os.fdopen(sys.stderr.fileno(), "w", buffering=1)


def log(msg):
    """Print message with immediate flush"""
    print(msg, flush=True)


# Add OSS fbthrift Python library to path
FBTHRIFT_PREFIX = Path(
    os.getenv("FBTHRIFT_PREFIX", "/home/wsu/proxygen/proxygen/_build/deps")
)
THRIFT_PY_PATH = FBTHRIFT_PREFIX / "lib" / "fb-py-libs" / "thrift_py"
if THRIFT_PY_PATH.exists():
    sys.path.insert(0, str(THRIFT_PY_PATH))

# Add generated Thrift bindings to path
GEN_PY_PATH = Path(__file__).parent / "build" / "gen-py3"
sys.path.insert(0, str(GEN_PY_PATH))

from mock_services import (
    MockAdsService,
    MockClipsDiscoverService,
    MockContentFilterService,
    MockInboxService,
    MockRankingService,
    MockReelsTrayService,
    MockUserPreferenceService,
)
from mock_services.ttypes import (
    AdInsertion,
    ClipChunk,
    ClipMedia,
    ClipsChunksResponse,
    ClipsDiscoverResponse,
    ClipsRankingResponse,
    FetchAdsRequest,
    FetchAdsResponse,
    FilterContentRequest,
    FilterContentResponse,
    InboxGetThreadsResponse,
    InboxMessagePreview,
    InboxMessagePreviewsResponse,
    InboxSpamCheckResponse,
    InboxSpamCheckResult,
    InboxThread,
    InboxUserMetadata,
    InboxUserMetadataResponse,
    IrisSubscriptionState,
    IrisSubscriptionStateResponse,
    RankItemsRequest,
    RankItemsResponse,
    ReelsTrayResponse,
    TrayBucket,
    TrayBucketClipsResponse,
    TrayPagingInfo,
    TrayRankingResponse,
    TrayReelItem,
    TrayUserMetadata,
    UserMetadataBatchResponse,
    UserPreferencesRequest,
    UserPreferencesResponse,
)
from thrift.protocol import TBinaryProtocol
from thrift.transport import TSocket, TTransport


class DualStackTServerSocket(TSocket.TServerSocket):
    """
    Custom TServerSocket that supports both IPv4 and IPv6 connections.

    Binds to IPv6 wildcard address (::) with IPV6_V6ONLY=0 to accept
    both IPv4-mapped IPv6 addresses and native IPv6 addresses.
    """

    def __init__(self, port: int):
        """Initialize dual-stack server socket on specified port."""
        self.port = port
        self.handle = None

    def listen(self):
        """Create and bind dual-stack socket."""
        # Create IPv6 socket
        self.handle = socket.socket(socket.AF_INET6, socket.SOCK_STREAM)

        # Enable address reuse
        self.handle.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

        # Enable dual-stack (accept both IPv4 and IPv6)
        # IPV6_V6ONLY=0 allows IPv4-mapped IPv6 addresses
        self.handle.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)

        # Bind to IPv6 wildcard address (::) which accepts all interfaces
        # IPv4 clients will connect as IPv4-mapped IPv6 addresses (::ffff:x.x.x.x)
        self.handle.bind(("::", self.port))

        # Start listening with large backlog for high concurrency
        # Backlog of 1024 allows many queued connections before rejecting
        self.handle.listen(1024)

        print(
            f"[DualStackSocket] Listening on [::]:{self.port} (IPv4 + IPv6, backlog=1024)"
        )

    def accept(self):
        """Accept connection from either IPv4 or IPv6 client."""
        if self.handle:
            client, addr = self.handle.accept()
            # Create TSocket from accepted client socket
            tsocket = TSocket.TSocket()
            tsocket.setHandle(client)
            return tsocket
        return None

    def close(self):
        """Close server socket."""
        if self.handle:
            self.handle.close()
            self.handle = None


class MockContentFilterServiceHandler:
    """
    Handler implementation for MockContentFilterService.

    Each RPC call creates Python↔Thrift boundary crossings for content filtering.
    """

    def filterContent(self, request: FilterContentRequest) -> FilterContentResponse:
        """
        Filters content based on safety rules.

        Creates RPC overhead through:
        - Thrift deserialization (request with item list)
        - Content filtering logic
        - Thrift serialization (response with filtered items)
        """
        item_ids = request.item_ids
        # user_id and filter_level from request are available for filtering logic
        # Currently using random filtering for benchmarking purposes

        # Randomly filter out ~10% of items
        safe_items = []
        blocked_items = []

        for item_id in item_ids:
            if random.random() > 0.1:  # 90% pass filter
                safe_items.append(item_id)
            else:
                blocked_items.append(item_id)

        response = FilterContentResponse(
            safe_item_ids=safe_items,
            blocked_item_ids=blocked_items,
            total_filtered=len(blocked_items),
            request_id=f"filter_req_{random.randint(1000, 9999)}",
        )

        return response


class MockUserPreferenceServiceHandler:
    """
    Handler implementation for MockUserPreferenceService.

    Each RPC call creates Python↔Thrift boundary crossings for user preferences.
    """

    def getUserPreferences(
        self, request: UserPreferencesRequest
    ) -> UserPreferencesResponse:
        """
        Fetches user preferences for personalization.

        Creates RPC overhead through:
        - Thrift deserialization (request)
        - Preference generation
        - Thrift serialization (response with preferences map)
        """
        # user_id from request is available for preference lookup
        # Currently using random preferences for benchmarking purposes

        # Generate random preferences
        preferences = {
            "video_affinity": random.random(),
            "photo_affinity": random.random(),
            "text_affinity": random.random(),
            "reels_affinity": random.random(),
            "explore_affinity": random.random(),
        }

        favorite_topics = random.sample(
            ["sports", "food", "travel", "fashion", "tech", "music", "art"], k=3
        )

        response = UserPreferencesResponse(
            preferences=preferences,
            favorite_topics=favorite_topics,
            request_id=f"pref_req_{random.randint(1000, 9999)}",
        )

        return response


class MockRankingServiceHandler:
    """
    Handler implementation for MockRankingService.

    Each RPC call creates Python↔Thrift boundary crossings for ranking operations.
    """

    def rankItems(self, request: RankItemsRequest) -> RankItemsResponse:
        """
        Ranks items based on user preferences.

        Creates RPC overhead through:
        - Thrift deserialization (request with item list)
        - Random ranking computation
        - Thrift serialization (response with scores)
        """
        # user_id from request is available for personalized ranking
        # Currently using random ranking for benchmarking purposes
        item_ids = request.item_ids
        num_results = min(request.num_results, len(item_ids))

        # Generate random ranking scores and shuffle items
        scored_items = [(item_id, random.random() * 100) for item_id in item_ids]
        scored_items.sort(key=lambda x: x[1], reverse=True)

        # Return top N ranked items
        ranked_items = scored_items[:num_results]

        response = RankItemsResponse(
            item_ids=[item[0] for item in ranked_items],
            scores=[item[1] for item in ranked_items],
            request_id=f"rank_req_{random.randint(1000, 9999)}",
        )

        return response


class MockAdsServiceHandler:
    """
    Handler implementation for MockAdsService.

    Each RPC call triggers:
    1. Thrift deserialization (request)
    2. Python object creation (80 fields × N ads)
    3. Thrift serialization (response)

    This creates Python↔Thrift boundary crossings!
    """

    def fetchAds(self, request: FetchAdsRequest) -> FetchAdsResponse:
        """
        Fetches mock ads with 80 fields each.

        Creates significant serialization overhead:
        - 80 fields per ad × num_ads_requested
        - Thrift type inspection and encoding
        - Memory allocation and copying
        """
        num_ads = request.num_ads_requested

        # Generate ads with production-scale data
        ads = []
        for _ in range(num_ads):
            ad = self._create_ad()
            ads.append(ad)

        response = FetchAdsResponse(
            ads=ads,
            total_fetched=len(ads),
            request_id=f"req_{random.randint(1000, 9999)}",
        )

        return response

    def _create_ad(self) -> AdInsertion:
        """
        Creates a simplified AdInsertion with 30 fields.

        Much faster generation - no nested objects, no large arrays, no binary data!
        """
        ad_id = random.randint(1000000, 9999999)

        return AdInsertion(
            # Core identifiers (10 fields)
            ad_id=ad_id,
            campaign_id=random.randint(100000, 999999),
            creative_id=random.randint(10000, 99999),
            advertiser_id=random.randint(1000, 9999),
            tracking_token=f"tk_{ad_id}",
            impression_id=f"imp_{ad_id}",
            ad_title=f"Ad {ad_id}",
            ad_subtitle="Limited Time Offer",
            call_to_action="SHOP_NOW",
            destination_url=f"https://example.com/ad/{ad_id}",
            # Engagement metrics (5 fields)
            view_count=random.randint(0, 100000),
            like_count=random.randint(0, 10000),
            comment_count=random.randint(0, 1000),
            share_count=random.randint(0, 500),
            is_video=random.choice([True, False]),
            # Ranking scores (10 fields)
            quality_score=random.random(),
            predicted_ctr=random.random() * 0.1,
            predicted_cvr=random.random() * 0.05,
            relevance_score=random.random(),
            engagement_score=random.random(),
            brand_safety_score=random.random(),
            user_affinity_score=random.random(),
            content_quality_score=random.random(),
            viewability_score=random.random(),
            completion_rate=random.random(),
            # Media info (5 fields)
            image_url=f"https://cdn.example.com/img_{ad_id}.jpg",
            media_type="PHOTO",
            video_duration=random.randint(5, 60),
            surface_type="FEED",
            placement_type="IN_STREAM",
        )


class MockClipsDiscoverServiceHandler:
    """
    Handler implementation for MockClipsDiscoverService.

    Models the clips.api.views.async_stream_clips_discover endpoint from
    production IG Django server. Each RPC call creates Python↔Thrift
    boundary crossings for clips discovery operations.
    """

    def discoverClips(self, request) -> ClipsDiscoverResponse:
        """
        Discovers clips for the Reels tab.

        Creates RPC overhead through:
        - Thrift deserialization (request with parameters)
        - Clips generation and ranking
        - Ads fetching and blending
        - Thrift serialization (response with clips and ads)
        """
        user_id = request.user_id
        num_clips = request.num_clips_requested
        include_ads = request.include_ads

        # Generate mock clips
        clips = []
        for i in range(num_clips):
            clip = self._create_clip(i, user_id)
            clips.append(clip)

        # Generate ads if requested
        ads = []
        if include_ads:
            num_ads = max(3, num_clips // 5)
            for _ in range(num_ads):
                ad = self._create_ad_for_clips()
                ads.append(ad)

        response = ClipsDiscoverResponse(
            clips=clips,
            ads=ads,
            total_clips=len(clips),
            next_max_id=f"max_{random.randint(1000, 9999)}",
            more_available=True,
            request_id=f"clips_req_{random.randint(1000, 9999)}",
        )

        return response

    def rankClips(self, request) -> ClipsRankingResponse:
        """
        Ranks clips based on user preferences.

        Creates RPC overhead through:
        - Thrift deserialization (request with clip IDs)
        - Random ranking computation
        - Thrift serialization (response with ranked IDs and scores)
        """
        clip_ids = request.clip_ids
        num_results = min(request.num_results, len(clip_ids))

        # Generate random ranking scores
        scored_clips = [(clip_id, random.random() * 100) for clip_id in clip_ids]
        scored_clips.sort(key=lambda x: x[1], reverse=True)

        ranked_clips = scored_clips[:num_results]

        response = ClipsRankingResponse(
            ranked_clip_ids=[clip[0] for clip in ranked_clips],
            scores=[clip[1] for clip in ranked_clips],
            request_id=f"rank_clips_req_{random.randint(1000, 9999)}",
        )

        return response

    def getClipsChunks(self, request) -> ClipsChunksResponse:
        """
        Gets video chunks for progressive streaming.

        Creates RPC overhead through:
        - Thrift deserialization (request with video ID and chunk range)
        - Chunk metadata generation
        - Thrift serialization (response with chunk list)
        """
        video_id = request.video_id
        start_chunk = request.start_chunk
        num_chunks = request.num_chunks
        resolution = request.resolution or "1080p"

        # Generate mock chunks
        chunks = []
        chunk_duration_ms = 2000  # 2 seconds per chunk

        for i in range(num_chunks):
            chunk_index = start_chunk + i
            chunk = ClipChunk(
                chunk_id=random.randint(1000000, 9999999),
                video_id=video_id,
                chunk_index=chunk_index,
                chunk_url=f"https://cdn.example.com/clips/{video_id}/chunk_{chunk_index}.mp4",
                chunk_size_bytes=random.randint(100000, 2000000),
                duration_ms=chunk_duration_ms,
                start_time_ms=chunk_index * chunk_duration_ms,
                end_time_ms=(chunk_index + 1) * chunk_duration_ms,
                resolution=resolution,
                bitrate_kbps=random.randint(2000, 8000),
            )
            chunks.append(chunk)

        response = ClipsChunksResponse(
            chunks=chunks,
            total_chunks=40,  # Assume 40 total chunks
            request_id=f"chunks_req_{random.randint(1000, 9999)}",
        )

        return response

    def _create_clip(self, index: int, user_id: int) -> ClipMedia:
        """Creates a mock ClipMedia object."""
        clip_id = random.randint(1000000, 9999999)

        hashtag_options = [
            "trending",
            "viral",
            "fyp",
            "reels",
            "explore",
            "funny",
            "dance",
            "music",
        ]

        return ClipMedia(
            clip_id=clip_id,
            owner_id=random.randint(1000, 99999),
            title=f"Awesome Clip {index}",
            description=f"Check out this amazing clip #{clip_id}",
            duration_ms=random.randint(5000, 90000),
            view_count=random.randint(100, 10000000),
            like_count=random.randint(10, 1000000),
            comment_count=random.randint(0, 50000),
            share_count=random.randint(0, 10000),
            thumbnail_url=f"https://cdn.example.com/clips/{clip_id}/thumb.jpg",
            content_type=random.choice(["reel", "short_video", "clip"]),
            quality_score=random.random(),
            engagement_score=random.random(),
            hashtags=random.sample(hashtag_options, k=random.randint(2, 5)),
            is_ad=False,
        )

    def _create_ad_for_clips(self) -> AdInsertion:
        """Creates a mock AdInsertion for clips blending."""
        ad_id = random.randint(1000000, 9999999)

        return AdInsertion(
            ad_id=ad_id,
            campaign_id=random.randint(100000, 999999),
            creative_id=random.randint(10000, 99999),
            advertiser_id=random.randint(1000, 9999),
            tracking_token=f"clips_tk_{ad_id}",
            impression_id=f"clips_imp_{ad_id}",
            ad_title=f"Sponsored Clip {ad_id}",
            ad_subtitle="Discover more",
            call_to_action="LEARN_MORE",
            destination_url=f"https://example.com/clips_ad/{ad_id}",
            view_count=random.randint(0, 100000),
            like_count=random.randint(0, 10000),
            comment_count=random.randint(0, 1000),
            share_count=random.randint(0, 500),
            is_video=True,  # Clips ads are typically video
            quality_score=random.random(),
            predicted_ctr=random.random() * 0.1,
            predicted_cvr=random.random() * 0.05,
            relevance_score=random.random(),
            engagement_score=random.random(),
            brand_safety_score=random.random(),
            user_affinity_score=random.random(),
            content_quality_score=random.random(),
            viewability_score=random.random(),
            completion_rate=random.random(),
            image_url=f"https://cdn.example.com/clips_ad_{ad_id}.jpg",
            media_type="VIDEO",
            video_duration=random.randint(15, 60),
            surface_type="CLIPS",
            placement_type="IN_STREAM",
        )


class MockInboxServiceHandler:
    """
    Handler implementation for MockInboxService.

    Models the activity.api.views.inbox endpoint from production IG Django
    server. This service handles the Instagram Direct inbox - the messaging
    feature that allows users to send and receive direct messages.

    Key production patterns modeled:
    - Thread list fetching with pagination
    - Message preview aggregation
    - Spam filtering via microservice
    - User metadata fetching via NodeAPI/LazyUserDict
    - Iris subscription for real-time updates
    - Read state management and badge calculation

    Each RPC call creates Python↔Thrift boundary crossings for realistic
    I-cache pressure simulation.
    """

    # Configuration matching production patterns
    MAX_THREADS_PER_PAGE = 20
    MESSAGE_PREVIEW_LENGTH = 50

    def getThreads(self, request) -> InboxGetThreadsResponse:
        """
        Gets inbox threads for a viewer.

        Models fetching thread lists from Direct cache and database.
        Includes pagination support via cursor.

        Creates RPC overhead through:
        - Thrift deserialization (request with viewer info)
        - Thread generation with metadata
        - Thrift serialization (response with threads and paging)
        """
        viewer_id = request.viewer_id
        page_size = min(request.page_size, self.MAX_THREADS_PER_PAGE)
        include_spam = request.include_spam
        current_time = int(time.time())

        # Generate mock threads
        threads = []
        for i in range(page_size):
            thread_id = f"thread_{viewer_id}_{i}_{random.randint(1000, 9999)}"
            num_participants = random.randint(2, 8)
            participant_ids = [
                f"user_{random.randint(1, 10000)}" for _ in range(num_participants)
            ]

            is_spam = random.random() < 0.05 if include_spam else False
            thread_types = ["private", "group"]

            thread = InboxThread(
                thread_id=thread_id,
                participant_ids=participant_ids,
                last_activity_at=current_time - random.randint(0, 86400 * 7),
                unread_count=random.randint(0, 10) if random.random() < 0.3 else 0,
                is_spam=is_spam,
                is_muted=random.random() < 0.1,
                thread_type=random.choice(thread_types),
                title=f"Chat {i}" if random.random() < 0.3 else None,
            )
            threads.append(thread)

        # Sort by last activity
        threads.sort(key=lambda t: t.last_activity_at, reverse=True)

        # Generate next cursor
        next_cursor = None
        if threads:
            next_cursor = f"cursor_{threads[-1].thread_id}"

        response = InboxGetThreadsResponse(
            threads=threads,
            total_threads=len(threads),
            next_cursor=next_cursor,
            has_more=True,
            request_id=f"inbox_threads_req_{random.randint(1000, 9999)}",
        )

        return response

    def getMessagePreviews(self, request) -> InboxMessagePreviewsResponse:
        """
        Gets message previews for threads.

        Models fetching the latest messages for inbox display.

        Creates RPC overhead through:
        - Thrift deserialization (request with thread IDs)
        - Message preview generation
        - Thrift serialization (response with previews map)
        """
        thread_ids = request.thread_ids
        messages_per_thread = request.messages_per_thread
        current_time = int(time.time())

        message_types = ["text", "media", "link", "voice", "video_call"]
        sample_texts = [
            "Hey, how are you?",
            "Check this out!",
            "Thanks!",
            "See you later",
            "Sounds good",
            "👍",
            "😂",
            "Sent a photo",
            "Shared a reel",
            "Voice message",
        ]

        previews = {}
        for thread_id in thread_ids:
            messages = []
            for m in range(messages_per_thread):
                message = InboxMessagePreview(
                    message_id=f"msg_{thread_id}_{m}_{random.randint(1000, 9999)}",
                    thread_id=thread_id,
                    sender_id=f"user_{random.randint(1, 10000)}",
                    text_preview=random.choice(sample_texts),
                    timestamp=current_time - random.randint(0, 3600),
                    message_type=random.choice(message_types),
                    is_unsent=random.random() < 0.02,
                )
                messages.append(message)
            previews[thread_id] = messages

        response = InboxMessagePreviewsResponse(
            previews=previews,
            request_id=f"inbox_previews_req_{random.randint(1000, 9999)}",
        )

        return response

    def checkThreadsSpam(self, request) -> InboxSpamCheckResponse:
        """
        Checks threads for spam.

        Models calling the spam filtering microservice.

        Creates RPC overhead through:
        - Thrift deserialization (request with thread IDs)
        - Spam score computation
        - Thrift serialization (response with spam results)
        """
        thread_ids = request.thread_ids
        spam_reasons = [
            "suspicious_links",
            "mass_messaging",
            "keyword_match",
            "new_account",
            "reported_user",
        ]

        results = {}
        for thread_id in thread_ids:
            spam_score = random.random()
            is_spam = spam_score > 0.8

            result = InboxSpamCheckResult(
                thread_id=thread_id,
                is_spam=is_spam,
                spam_score=spam_score,
                spam_reason=random.choice(spam_reasons) if is_spam else None,
            )
            results[thread_id] = result

        response = InboxSpamCheckResponse(
            results=results,
            request_id=f"inbox_spam_req_{random.randint(1000, 9999)}",
        )

        return response

    def getUserMetadata(self, request) -> InboxUserMetadataResponse:
        """
        Gets user metadata for inbox participants.

        Models the NodeAPI/LazyUserDict pattern for batch user fetching.

        Creates RPC overhead through:
        - Thrift deserialization (request with user IDs)
        - User metadata generation
        - Thrift serialization (response with metadata map)
        """
        user_ids = request.user_ids
        current_time = int(time.time())
        presence_statuses = ["active", "recently_active", "offline"]

        sample_usernames = [
            "alice",
            "bob",
            "charlie",
            "diana",
            "emma",
            "frank",
            "grace",
            "henry",
            "ivy",
            "jack",
        ]
        sample_names = [
            "Alice Smith",
            "Bob Jones",
            "Charlie Brown",
            "Diana Prince",
            "Emma Watson",
            "Frank Miller",
            "Grace Lee",
            "Henry Ford",
            "Ivy Chen",
            "Jack Wilson",
        ]

        metadata = {}
        for i, user_id in enumerate(user_ids):
            username = sample_usernames[i % len(sample_usernames)]
            full_name = sample_names[i % len(sample_names)]

            user_meta = InboxUserMetadata(
                user_id=user_id,
                username=f"{username}_{user_id[-4:]}",
                full_name=full_name,
                profile_pic_url=f"https://cdn.example.com/pics/{user_id}.jpg",
                is_verified=random.random() < 0.05,
                is_private=random.random() < 0.3,
                presence_status=random.choice(presence_statuses),
                last_active_at=current_time - random.randint(0, 3600)
                if random.random() < 0.5
                else None,
            )
            metadata[user_id] = user_meta

        response = InboxUserMetadataResponse(
            metadata=metadata,
            total_fetched=len(metadata),
            request_id=f"inbox_meta_req_{random.randint(1000, 9999)}",
        )

        return response

    def getIrisState(self, request) -> IrisSubscriptionStateResponse:
        """
        Gets Iris subscription state for real-time inbox updates.

        Models checking Iris for pending updates and resnapshot triggers.

        Creates RPC overhead through:
        - Thrift deserialization (request with viewer ID)
        - Subscription state generation
        - Thrift serialization (response with Iris state)
        """
        viewer_id = request.viewer_id
        current_time = int(time.time())
        has_updates = random.random() < 0.2

        pending_threads = []
        if has_updates:
            num_pending = random.randint(1, 5)
            pending_threads = [
                f"thread_{viewer_id}_{random.randint(1000, 9999)}"
                for _ in range(num_pending)
            ]

        state = IrisSubscriptionState(
            sequence_id=random.randint(1000000, 9999999),
            snapshot_at=current_time - random.randint(0, 300),
            has_pending_updates=has_updates,
            pending_thread_ids=pending_threads,
        )

        response = IrisSubscriptionStateResponse(
            state=state,
            request_id=f"pubsub_state_req_{random.randint(1000, 9999)}",
        )

        return response


class MockReelsTrayServiceHandler:
    """
    Handler implementation for MockReelsTrayService.

    Models the feed.api.views.reels_tray endpoint from production IG Django
    server. This service handles the stories/reels tray - the horizontal
    scrollable bar at the top of the Instagram feed showing profile bubbles
    for users with active Stories/Reels.

    Key production patterns modeled:
    - RankedTrayCache: Caching of ranked tray results
    - IGML Pipelines (Shots/Brewery/Barkeep): ML-based ranking
    - NodeAPI/LazyUserDict: User metadata fetching
    - Partial Materialization: First N buckets fully filled, rest skeletons

    Each RPC call creates Python↔Thrift boundary crossings for realistic
    I-cache pressure simulation.
    """

    # Configuration matching production patterns
    NUM_FILLED_BUCKETS = 4  # First N buckets are fully materialized
    MAX_ITEMS_PER_BUCKET = 10  # Maximum stories/reels per bucket

    def getTray(self, request) -> ReelsTrayResponse:
        """
        Gets the stories/reels tray for a viewer.

        Models the full production flow:
        1. Check RankedTrayCache for prefetched results
        2. Source candidate users with active stories
        3. Rank via IGML pipelines (Shots/Brewery/Barkeep)
        4. Fetch user metadata via NodeAPI/LazyUserDict
        5. Build buckets with partial materialization
        6. Insert self story and live stories
        7. Cache results for future requests

        Creates RPC overhead through:
        - Thrift deserialization (request with viewer info)
        - Bucket and item generation with partial materialization
        - Thrift serialization (response with buckets and paging info)
        """
        viewer_id = request.viewer_id
        num_buckets = request.num_buckets_requested
        include_live = request.include_live
        include_self = request.include_self_story
        items_per_bucket = request.num_items_per_bucket or self.MAX_ITEMS_PER_BUCKET

        # Generate tray buckets with partial materialization
        buckets = []
        unseen_count = 0

        for i in range(num_buckets):
            # Only first N buckets are fully materialized
            is_materialized = i < self.NUM_FILLED_BUCKETS

            bucket = self._create_tray_bucket(
                index=i,
                viewer_id=viewer_id,
                is_materialized=is_materialized,
                items_per_bucket=items_per_bucket if is_materialized else 0,
            )
            buckets.append(bucket)

            if bucket.user_metadata.has_unseen_stories:
                unseen_count += 1

        # Create self bucket if requested
        self_bucket = None
        if include_self:
            self_bucket = self._create_tray_bucket(
                index=-1,
                viewer_id=viewer_id,
                is_materialized=True,
                items_per_bucket=items_per_bucket,
                is_self=True,
            )

        # Create paging info
        paging_info = TrayPagingInfo(
            max_id=f"tray_max_{random.randint(1000, 9999)}",
            more_available=True,
            prefetch_count=self.NUM_FILLED_BUCKETS,
            next_cursor=f"cursor_{random.randint(1000, 9999)}",
        )

        response = ReelsTrayResponse(
            buckets=buckets,
            paging_info=paging_info,
            total_buckets=len(buckets),
            num_materialized=min(self.NUM_FILLED_BUCKETS, len(buckets)),
            request_id=f"tray_req_{random.randint(1000, 9999)}",
            has_self_story=include_self,
            unseen_count=unseen_count,
            self_bucket=self_bucket,
        )

        return response

    def rankTrayUsers(self, request) -> TrayRankingResponse:
        """
        Ranks users for tray positioning via IGML pipelines.

        Models the Shots/Brewery/Barkeep ML ranking system that determines
        which users appear first in the tray based on:
        - User affinity scores
        - Engagement history
        - Content freshness
        - Live status

        Creates RPC overhead through:
        - Thrift deserialization (request with candidate users)
        - Ranking score computation
        - Thrift serialization (response with ranked users and scores)
        """
        viewer_id = request.viewer_id
        candidate_ids = request.candidate_user_ids
        num_results = min(request.num_results, len(candidate_ids))

        # Generate ranking scores for candidates
        scored_users = [(user_id, random.random() * 100) for user_id in candidate_ids]
        scored_users.sort(key=lambda x: x[1], reverse=True)

        ranked_users = scored_users[:num_results]

        response = TrayRankingResponse(
            ranked_user_ids=[user[0] for user in ranked_users],
            ranking_scores=[user[1] for user in ranked_users],
            request_id=f"tray_rank_req_{random.randint(1000, 9999)}",
            model_version="shots_v3.2",
        )

        return response

    def getUserMetadataBatch(self, request) -> UserMetadataBatchResponse:
        """
        Fetches user metadata in batch via NodeAPI/LazyUserDict pattern.

        Models the production pattern where user metadata is fetched
        lazily and in batches to minimize database round-trips. Includes:
        - Profile information (username, pic, verified status)
        - Story/Reel counts and freshness
        - Live status
        - Relationship info (close friends, favorites)

        Creates RPC overhead through:
        - Thrift deserialization (request with user IDs)
        - Metadata generation for each user
        - Thrift serialization (response with metadata map)
        """
        viewer_id = request.viewer_id
        user_ids = request.user_ids
        include_story_info = request.include_story_info
        include_live_info = request.include_live_info

        # Generate metadata for each user
        metadata_map = {}
        for user_id in user_ids:
            metadata = self._create_user_metadata(
                user_id=user_id,
                include_story_info=include_story_info,
                include_live_info=include_live_info,
            )
            metadata_map[user_id] = metadata

        response = UserMetadataBatchResponse(
            user_metadata=metadata_map,
            request_id=f"meta_batch_req_{random.randint(1000, 9999)}",
            total_fetched=len(metadata_map),
        )

        return response

    def getTrayBucketClips(self, request) -> TrayBucketClipsResponse:
        """
        Gets clips for a specific tray bucket (lazy loading).

        Models the partial materialization pattern where skeleton buckets
        can be filled on-demand when the user scrolls to them. This reduces
        initial load time while maintaining smooth scrolling experience.

        Creates RPC overhead through:
        - Thrift deserialization (request with bucket info)
        - Item generation for the bucket
        - Thrift serialization (response with items)
        """
        viewer_id = request.viewer_id
        bucket_user_id = request.bucket_user_id
        num_items = request.num_items

        # Generate items for this bucket
        items = []
        for i in range(num_items):
            item = self._create_reel_item(
                index=i,
                owner_id=bucket_user_id,
            )
            items.append(item)

        response = TrayBucketClipsResponse(
            items=items,
            total_items=len(items),
            more_available=random.choice([True, False]),
            request_id=f"bucket_clips_req_{random.randint(1000, 9999)}",
        )

        return response

    def _create_tray_bucket(
        self,
        index: int,
        viewer_id: int,
        is_materialized: bool,
        items_per_bucket: int,
        is_self: bool = False,
    ) -> TrayBucket:
        """Creates a mock TrayBucket with optional full materialization."""
        bucket_id = random.randint(1000000, 9999999)
        user_id = viewer_id if is_self else random.randint(1000, 99999)

        # Create user metadata
        user_metadata = self._create_user_metadata(
            user_id=user_id,
            include_story_info=True,
            include_live_info=True,
        )

        # Create items only if materialized
        items = []
        if is_materialized and items_per_bucket > 0:
            for i in range(items_per_bucket):
                item = self._create_reel_item(index=i, owner_id=user_id)
                items.append(item)

        return TrayBucket(
            bucket_id=bucket_id,
            user_id=user_id,
            user_metadata=user_metadata,
            items=items,
            item_count=len(items) if is_materialized else random.randint(1, 10),
            is_materialized=is_materialized,
            seen_at=random.randint(0, 86400000) if random.random() > 0.5 else 0,
            ranking_score=random.random() * 100,
            position=index,
            bucket_type="self" if is_self else random.choice(["story", "reel", "live"]),
        )

    def _create_user_metadata(
        self,
        user_id: int,
        include_story_info: bool = True,
        include_live_info: bool = True,
    ) -> TrayUserMetadata:
        """Creates mock user metadata for tray display."""
        return TrayUserMetadata(
            user_id=user_id,
            username=f"user_{user_id}",
            full_name=f"User {user_id}",
            profile_pic_url=f"https://cdn.example.com/profiles/{user_id}.jpg",
            is_verified=random.random() > 0.9,
            has_unseen_stories=random.random() > 0.3 if include_story_info else False,
            story_count=random.randint(1, 15) if include_story_info else 0,
            reel_count=random.randint(0, 50) if include_story_info else 0,
            is_live=random.random() > 0.95 if include_live_info else False,
            latest_reel_timestamp=random.randint(1700000000, 1702000000),
            is_close_friend=random.random() > 0.8,
            is_favorite=random.random() > 0.85,
            affinity_score=random.random(),
            has_besties_media=random.random() > 0.9,
            ring_color=random.choice(["gradient", "green", "rainbow", ""]),
        )

    def _create_reel_item(self, index: int, owner_id: int) -> TrayReelItem:
        """Creates a mock reel/story item."""
        item_id = random.randint(1000000, 9999999)

        hashtag_options = [
            "story",
            "reel",
            "viral",
            "trending",
            "fyp",
            "daily",
            "life",
            "fun",
        ]

        return TrayReelItem(
            item_id=item_id,
            owner_id=owner_id,
            media_type=random.choice(["story", "reel", "highlight"]),
            duration_ms=random.randint(3000, 60000),
            thumbnail_url=f"https://cdn.example.com/stories/{item_id}/thumb.jpg",
            video_url=f"https://cdn.example.com/stories/{item_id}/video.mp4",
            taken_at=random.randint(1700000000, 1702000000),
            expiring_at=random.randint(1702000000, 1702100000),
            is_seen=random.random() > 0.5,
            seen_at=random.randint(1700000000, 1702000000)
            if random.random() > 0.5
            else 0,
            view_count=random.randint(10, 100000),
            reply_count=random.randint(0, 1000),
            has_audio=random.random() > 0.2,
            audio_track_id=f"audio_{random.randint(1000, 9999)}",
            hashtags=random.sample(hashtag_options, k=random.randint(0, 3)),
        )


def main():
    """Start the Thrift RPC server with all mock services."""
    # Server configuration
    # Bind to 0.0.0.0 to accept connections from any network interface (including remote hosts)
    HOST = "0.0.0.0"
    PORT = int(os.getenv("THRIFT_PORT", "9090"))  # Allow port override via env var
    MAX_WORKERS = 200  # Increased from 50 to 200 for higher concurrency

    print("[ThriftServer] Initializing server components...")

    # Create handlers for all services
    ads_handler = MockAdsServiceHandler()
    ranking_handler = MockRankingServiceHandler()
    filter_handler = MockContentFilterServiceHandler()
    pref_handler = MockUserPreferenceServiceHandler()
    clips_handler = MockClipsDiscoverServiceHandler()
    reels_tray_handler = MockReelsTrayServiceHandler()
    inbox_handler = MockInboxServiceHandler()

    print("[ThriftServer] Created handlers for all services")

    # Create server transport using dual-stack socket (IPv4 + IPv6)
    transport = DualStackTServerSocket(port=PORT)
    tfactory = TTransport.TBufferedTransportFactory()
    pfactory = TBinaryProtocol.TBinaryProtocolFactory()
    print("[ThriftServer] Created dual-stack transport and factories")

    print(f"[ThriftServer] Starting Thrift server on {HOST}:{PORT}")
    print(f"[ThriftServer] Thread pool size: {MAX_WORKERS} concurrent connections")
    print(
        "[ThriftServer] Supporting 7 services: Ads, Ranking, ContentFilter, "
        "UserPreference, ClipsDiscover, ReelsTray, Inbox"
    )
    print("[ThriftServer] Each RPC creates Python↔Thrift boundary crossings")
    print("[ThriftServer] Server accepts connections from any network interface")
    print("[ThriftServer] Press Ctrl+C to stop")

    # Create thread pool executor for handling concurrent connections
    executor = ThreadPoolExecutor(
        max_workers=MAX_WORKERS, thread_name_prefix="ThriftWorker"
    )

    try:
        # Open/listen on the transport
        print("[ThriftServer] Calling transport.listen()...")
        transport.listen()
        print("[ThriftServer] Transport is listening")

        # Serve connections in a loop with thread pool
        print(
            f"[ThriftServer] Starting multi-threaded server loop (thread pool size: {MAX_WORKERS})..."
        )
        while True:
            client = transport.accept()
            if client:
                # Submit client handling to thread pool
                executor.submit(
                    handle_client,
                    client,
                    tfactory,
                    pfactory,
                    ads_handler,
                    ranking_handler,
                    filter_handler,
                    pref_handler,
                    clips_handler,
                    reels_tray_handler,
                    inbox_handler,
                )

    except KeyboardInterrupt:
        print("\n[ThriftServer] Server shutting down...")
    except Exception as e:
        print(f"[ThriftServer] ERROR: {e}")
        import traceback

        traceback.print_exc()
        raise
    finally:
        print("[ThriftServer] Shutting down thread pool...")
        executor.shutdown(wait=True, cancel_futures=False)
        print("[ThriftServer] Thread pool shut down")
        transport.close()


def handle_client(
    client,
    tfactory,
    pfactory,
    ads_handler,
    ranking_handler,
    filter_handler,
    pref_handler,
    clips_handler,
    reels_tray_handler,
    inbox_handler,
):
    """Handle a single client connection with all services."""
    try:
        itrans = tfactory.getTransport(client)
        otrans = tfactory.getTransport(client)
        iprot = pfactory.getProtocol(itrans)
        oprot = pfactory.getProtocol(otrans)

        # Create processors for all services
        ads_processor = MockAdsService.Processor(ads_handler)
        ranking_processor = MockRankingService.Processor(ranking_handler)
        filter_processor = MockContentFilterService.Processor(filter_handler)
        pref_processor = MockUserPreferenceService.Processor(pref_handler)
        clips_processor = MockClipsDiscoverService.Processor(clips_handler)
        reels_tray_processor = MockReelsTrayService.Processor(reels_tray_handler)
        inbox_processor = MockInboxService.Processor(inbox_handler)

        try:
            while True:
                # Read the message name to determine which service to use
                (fname, mtype, rseqid) = iprot.readMessageBegin()

                # Decode bytes to string if necessary (OSS fbthrift returns bytes)
                method_name = (
                    fname.decode("utf-8") if isinstance(fname, bytes) else fname
                )

                # Route to appropriate processor based on method name
                # OSS fbthrift requires server_ctx parameter (pass None)
                if method_name in ["fetchAds"]:
                    iprot.readMessageEnd()
                    ads_processor.process_fetchAds(rseqid, iprot, oprot, None)
                elif method_name in ["rankItems"]:
                    iprot.readMessageEnd()
                    ranking_processor.process_rankItems(rseqid, iprot, oprot, None)
                elif method_name in ["filterContent"]:
                    iprot.readMessageEnd()
                    filter_processor.process_filterContent(rseqid, iprot, oprot, None)
                elif method_name in ["getUserPreferences"]:
                    iprot.readMessageEnd()
                    pref_processor.process_getUserPreferences(
                        rseqid, iprot, oprot, None
                    )
                elif method_name in ["discoverClips"]:
                    iprot.readMessageEnd()
                    clips_processor.process_discoverClips(rseqid, iprot, oprot, None)
                elif method_name in ["rankClips"]:
                    iprot.readMessageEnd()
                    clips_processor.process_rankClips(rseqid, iprot, oprot, None)
                elif method_name in ["getClipsChunks"]:
                    iprot.readMessageEnd()
                    clips_processor.process_getClipsChunks(rseqid, iprot, oprot, None)
                # Reels Tray service methods
                elif method_name in ["getTray"]:
                    iprot.readMessageEnd()
                    reels_tray_processor.process_getTray(rseqid, iprot, oprot, None)
                elif method_name in ["rankTrayUsers"]:
                    iprot.readMessageEnd()
                    reels_tray_processor.process_rankTrayUsers(
                        rseqid, iprot, oprot, None
                    )
                elif method_name in ["getUserMetadataBatch"]:
                    iprot.readMessageEnd()
                    reels_tray_processor.process_getUserMetadataBatch(
                        rseqid, iprot, oprot, None
                    )
                elif method_name in ["getTrayBucketClips"]:
                    iprot.readMessageEnd()
                    reels_tray_processor.process_getTrayBucketClips(
                        rseqid, iprot, oprot, None
                    )
                # Inbox service methods
                elif method_name in ["getThreads"]:
                    iprot.readMessageEnd()
                    inbox_processor.process_getThreads(rseqid, iprot, oprot, None)
                elif method_name in ["getMessagePreviews"]:
                    iprot.readMessageEnd()
                    inbox_processor.process_getMessagePreviews(
                        rseqid, iprot, oprot, None
                    )
                elif method_name in ["checkThreadsSpam"]:
                    iprot.readMessageEnd()
                    inbox_processor.process_checkThreadsSpam(rseqid, iprot, oprot, None)
                elif method_name in ["getUserMetadata"]:
                    iprot.readMessageEnd()
                    inbox_processor.process_getUserMetadata(rseqid, iprot, oprot, None)
                elif method_name in ["getIrisState"]:
                    iprot.readMessageEnd()
                    inbox_processor.process_getIrisState(rseqid, iprot, oprot, None)
                else:
                    print(f"[ThriftServer] WARNING: Unknown method '{method_name}'")
                    iprot.skip(TBinaryProtocol.TType.STRUCT)
                    iprot.readMessageEnd()
                    x = TBinaryProtocol.TApplicationException(
                        TBinaryProtocol.TApplicationException.UNKNOWN_METHOD,
                        f"Unknown method {method_name}",
                    )
                    oprot.writeMessageBegin(
                        fname, TBinaryProtocol.TMessageType.EXCEPTION, rseqid
                    )
                    x.write(oprot)
                    oprot.writeMessageEnd()
                    oprot.trans.flush()

        except TTransport.TTransportException:
            pass  # Normal client disconnect
        except Exception as e:
            print(f"[ThriftServer] Error in message processing loop: {e}")
            import traceback

            traceback.print_exc()
    except Exception as e:
        print(f"[ThriftServer] Error setting up client handler: {e}")
        import traceback

        traceback.print_exc()
    finally:
        try:
            itrans.close()
            otrans.close()
        except Exception:
            pass  # Ignore errors during cleanup


if __name__ == "__main__":
    main()

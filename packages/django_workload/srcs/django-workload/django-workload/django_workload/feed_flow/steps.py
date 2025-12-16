# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
Concrete FeedFlow step implementations
Mimics various FeedFlow steps from IG Django: FetchAds, SourceAndRank, Timeline, etc.
"""

import copy
import hashlib
import logging
import random
from typing import Any, Dict
from uuid import UUID

from .step import FeedFlowStep
from .thrift_client import (
    AdData,
    get_ads_client,
    get_filter_client,
    get_preference_client,
    get_ranking_client,
)

logger = logging.getLogger(__name__)


def _uuid_to_int(uuid_obj: UUID) -> int:
    """Convert UUID to integer for Thrift RPC calls that expect i64."""
    return uuid_obj.int & 0x7FFFFFFFFFFFFFFF  # Convert to positive i64


class SourceAndRankStep(FeedFlowStep):
    """
    Sources and ranks content from database.
    Mimics IG's SourceAndRankStep which queries ranking service.
    """

    def enabled(self) -> bool:
        return self.context.enable_ranking

    def prepare(self) -> Dict[str, Any]:
        user = self.context.user
        feed_entries = user.feed_entries().limit(self.context.page_size * 2)

        # REMOVED: self._simulate_cpu_work(3) - reduces compute overhead
        organic_items = []
        for entry in feed_entries:
            item = {
                "pk": str(entry.id),
                "comment_count": entry.comment_count,
                "published": entry.published.timestamp(),
                "user": user.json_data,
                "score": random.random() * 100,
                "engagement_score": random.random() * 50,
                "relevance_score": random.random() * 75,
            }
            organic_items.append(item)

        # REDUCED RPC STRATEGY: 1 RPC call in SourceAndRankStep
        # Single ranking call to balance server load
        rpc_success = False
        try:
            ranking_client = get_ranking_client()

            # Convert UUID to int for Thrift RPC calls
            user_id_int = _uuid_to_int(user.id)

            # RPC CALL #1: Rank items with ML features
            response = ranking_client.rank_items(
                user_id=user_id_int,
                items=[item["pk"] for item in organic_items],
                num_results=len(organic_items),
            )

            # Apply ranking scores
            score_map = {
                item_id: score
                for item_id, score in zip(response.item_ids, response.scores)
            }
            for item in organic_items:
                item["score"] = score_map.get(item["pk"], item["score"])

            rpc_success = True
            logger.debug(
                "✅ SourceAndRankStep: Successfully completed 1 RPC call (ranking)"
            )
        except Exception as e:
            # Fallback if ranking service unavailable
            logger.warning(
                f"⚠️ SourceAndRankStep: RPC calls FAILED, using fallback logic. Error: {e}"
            )
            rpc_success = False

        return {"organic_items": organic_items}

    def run(self) -> None:
        if self._prepare_result:
            organic_items = self._prepare_result["organic_items"]

            ranked_items = self._simulate_ranking(organic_items, key="score")

            self.context.ranked_items = ranked_items
            self.context.source_and_rank_result = {
                "num_items": len(ranked_items),
                "avg_score": sum(item["score"] for item in ranked_items)
                / max(len(ranked_items), 1),
            }


class FetchAdsStep(FeedFlowStep):
    """
    Fetches ads and prepares for insertion.
    Mimics IG's FetchAdsStep which calls FeedAdsController.
    """

    def enabled(self) -> bool:
        return self.context.allow_ads and len(self.context.ranked_items) > 0

    def prepare(self) -> Dict[str, Any]:
        # PRODUCTION-SCALE RPC VOLUME: Fetch 50-100 ads with 10 batched RPC calls
        # This creates massive Thrift RPC overhead matching production patterns!
        num_ads = max(50, len(self.context.ranked_items) // 2)

        # CALL REAL THRIFT RPC SERVICE MULTIPLE TIMES IN BATCHES
        # This dramatically amplifies Python↔Thrift boundary crossings:
        # Each batch requires: Request serialization + RPC + Response deserialization
        try:
            ads_client = get_ads_client()
            ranking_client = get_ranking_client()
            filter_client = get_filter_client()
            pref_client = get_preference_client()

            # Convert UUID to int for Thrift RPC calls
            user_id_int = (
                _uuid_to_int(self.context.user.id)
                if hasattr(self.context, "user")
                else 1
            )

            # REDUCED RPC STRATEGY: 1 RPC call in FetchAdsStep
            # Single batch fetch to balance server load
            logger.debug(
                f"🚀 FetchAdsStep: Initiating {num_ads} ads fetch with 1 RPC call"
            )

            # RPC CALL #2: Fetch all ads in single batch
            response = ads_client.fetch_ads(user_id=user_id_int, num_ads=num_ads)

            # Convert Thrift AdInsertion objects to dict format
            ads = []
            for ad_thrift in response.ads:
                ad_data = AdData(ad_thrift)
                ads.append(ad_data.to_dict())

            logger.debug(
                f"✅ FetchAdsStep: Successfully completed 1 RPC call - fetched {response.total_fetched} ads with production-scale data (600+ fields/ad)"
            )

            return {
                "ads": ads,
                "ads_context": {
                    "num_ads_fetched": response.total_fetched,
                    "request_id": response.request_id,
                    "batches": 1,
                    "rpc_calls": 1,
                },
            }
        except Exception as e:
            # Fallback if Thrift server is not running
            logger.warning(
                f"⚠️ FetchAdsStep: RPC calls FAILED, using fallback (no ads). Error: {e}"
            )
            return {
                "ads": [],
                "ads_context": {
                    "num_ads_fetched": 0,
                    "request_id": "error",
                    "error": str(e),
                },
            }

    def run(self) -> None:
        if self._prepare_result:
            ads = self._prepare_result["ads"]
            self.context.ads_items = ads
            self.context.ads_context = self._prepare_result["ads_context"]


class InsertAdsStep(FeedFlowStep):
    """
    Inserts ads into organic content.
    Mimics ad insertion logic in IG's FeedFlow.
    """

    def enabled(self) -> bool:
        return len(self.context.ads_items) > 0 and len(self.context.ranked_items) > 0

    def run(self) -> None:
        merged_items = []
        ads = copy.deepcopy(self.context.ads_items)
        organic = copy.deepcopy(self.context.ranked_items)

        ads_index = 0
        ad_positions = []

        # REMOVED: self._simulate_cpu_work(2) - reduces compute overhead
        for idx, item in enumerate(organic):
            merged_items.append(item)

            if ads_index < len(ads) and (idx + 1) % 3 == 0:
                merged_items.append(ads[ads_index])
                ad_positions.append(len(merged_items) - 1)
                ads_index += 1

        # PRODUCTION-SCALE RPC STRATEGY: 4 RPC calls for ad insertion logic
        # This dramatically amplifies Python↔Thrift boundary crossings!
        try:
            ranking_client = get_ranking_client()
            filter_client = get_filter_client()

            # Convert UUID to int for Thrift RPC calls
            user_id_int = (
                _uuid_to_int(self.context.user.id)
                if hasattr(self.context, "user")
                else 1
            )

            # RPC CALL #14: Re-rank merged items (organic + ads)
            response = ranking_client.rank_items(
                user_id=user_id_int,
                items=[item["pk"] for item in merged_items],
                num_results=len(merged_items),
            )

            # Apply re-ranking scores
            score_map = {
                item_id: score
                for item_id, score in zip(response.item_ids, response.scores)
            }
            for item in merged_items:
                if item["pk"] in score_map:
                    item["score"] = score_map[item["pk"]]

            # RPC CALL #15: Apply diversity rules (simulates deduplication logic)
            diversity_response = ranking_client.rank_items(
                user_id=user_id_int,
                items=[item["pk"] for item in merged_items],
                num_results=len(merged_items),
            )

            # RPC CALL #16: Check ad position constraints (simulates placement validation)
            position_check = filter_client.filter_content(
                user_id=user_id_int,
                item_ids=[item["pk"] for item in merged_items],
                filter_level="moderate",
            )

            # RPC CALL #17: Final auction ranking (simulates final bid ordering)
            final_ranking = ranking_client.rank_items(
                user_id=user_id_int,
                items=[item["pk"] for item in merged_items],
                num_results=len(merged_items),
            )

            # Apply final ranking scores
            final_score_map = {
                item_id: score
                for item_id, score in zip(final_ranking.item_ids, final_ranking.scores)
            }
            for item in merged_items:
                if item["pk"] in final_score_map:
                    item["final_score"] = final_score_map[item["pk"]]

        except Exception:
            # Fallback if ranking/filtering services unavailable
            pass

        self.context.feed_items = merged_items
        self.context.add_metric("ad_positions", ad_positions)


class TimelineStep(FeedFlowStep):
    """
    Materializes final timeline response.
    Mimics IG's TimelineStep which converts media_info to media_dict.
    """

    def enabled(self) -> bool:
        return True

    def prepare(self) -> Dict[str, Any]:
        items = (
            self.context.feed_items
            if self.context.feed_items
            else self.context.ranked_items
        )

        # PRODUCTION-SCALE RPC STRATEGY: 5 RPC calls for timeline materialization
        # This dramatically amplifies Python↔Thrift boundary crossings!
        try:
            pref_client = get_preference_client()
            ranking_client = get_ranking_client()
            filter_client = get_filter_client()

            # Convert UUID to int for Thrift RPC calls
            user_id_int = (
                _uuid_to_int(self.context.user.id)
                if hasattr(self.context, "user")
                else 1
            )

            # RPC CALL #21: Get user preferences for personalized metadata
            prefs_response = pref_client.get_user_preferences(user_id=user_id_int)
            user_preferences = prefs_response.preferences

            # RPC CALL #22: Fetch personalization data (simulates additional user signals)
            personalization_data = pref_client.get_user_preferences(user_id=user_id_int)

            # RPC CALL #23: Get user demographics (simulates targeting data)
            demographics_response = ranking_client.rank_items(
                user_id=user_id_int,
                items=[str(i) for i in range(5)],  # Dummy demographic IDs
                num_results=5,
            )

            # RPC CALL #24: Fetch interaction history (simulates user engagement tracking)
            interaction_history = ranking_client.rank_items(
                user_id=user_id_int,
                items=[item["pk"] for item in items],
                num_results=len(items),
            )

            # RPC CALL #25: Get content affinities (simulates recommendation signals)
            affinity_data = filter_client.filter_content(
                user_id=user_id_int,
                item_ids=[item["pk"] for item in items],
                filter_level="relaxed",
            )
        except Exception:
            # Fallback if preference/ranking services unavailable
            user_preferences = {}
            personalization_data = None

        # REMOVED: self._simulate_cpu_work(3) and _simulate_serialization - reduces compute overhead
        materialized_items = []
        for item in items:
            materialized = {
                "id": item["pk"],
                "type": "ad" if item.get("is_ad") else "media",
                "comment_count": item["comment_count"],
                "timestamp": item["published"],
                "author": item["user"]["name"],
                # Add personalization from RPC response
                "personalization_score": user_preferences.get("video_affinity", 0.5)
                if "video" in str(item.get("type", ""))
                else user_preferences.get("photo_affinity", 0.5),
            }

            if item.get("is_ad"):
                materialized["ad_title"] = item.get("ad_title", "")

            materialized_items.append(materialized)

        return {"materialized_items": materialized_items}

    def run(self) -> None:
        if self._prepare_result:
            materialized_items = self._prepare_result["materialized_items"]

            self.context.timeline_response = {
                "num_results": len(materialized_items),
                "items": materialized_items,
                "feed_type": "main_feed",
            }


class TextFeedFlowStep(FeedFlowStep):
    """
    Text feed specific processing (Threads-like).
    Mimics IG's TextFeedFlowStepsBase for Threads.
    """

    def enabled(self) -> bool:
        return (
            hasattr(self.context.request, "text_feed_mode")
            and self.context.request.text_feed_mode
        )

    def prepare(self) -> Dict[str, Any]:
        items = self.context.ranked_items[:10]

        # REMOVED: self._simulate_cpu_work(4) and hashlib operations - reduces compute overhead
        text_items = []
        for item in items:
            text_item = copy.deepcopy(item)
            text_item["text_content"] = f"Text post {item['pk']}"
            text_item["thread_depth"] = random.randint(0, 3)
            text_item["reply_count"] = random.randint(0, 50)
            text_items.append(text_item)

        return {"text_items": text_items}

    def run(self) -> None:
        if self._prepare_result:
            text_items = self._prepare_result["text_items"]
            self.context.text_feed_result = {
                "items": text_items,
                "total_text_posts": len(text_items),
            }


class ReelsFeedFlowStep(FeedFlowStep):
    """
    Reels-specific ranking and processing.
    Mimics IG's Reels flow with clips ranking.
    """

    def enabled(self) -> bool:
        return (
            hasattr(self.context.request, "reels_mode")
            and self.context.request.reels_mode
        )

    def prepare(self) -> Dict[str, Any]:
        items = self.context.ranked_items[:15]

        # REMOVED: self._simulate_cpu_work(5) - reduces compute overhead
        reels_items = []
        for item in items:
            reel = copy.deepcopy(item)
            reel["reel_type"] = random.choice(
                ["short_video", "music_video", "tutorial"]
            )
            reel["duration_ms"] = random.randint(5000, 90000)
            reel["watch_time_score"] = random.random() * 100
            reel["completion_rate"] = random.random()

            reel["ranking_features"] = {
                "diversity_score": random.random(),
                "freshness_score": random.random(),
                "quality_score": random.random(),
            }

            reels_items.append(reel)

        return {
            "reels_items": self._simulate_ranking(reels_items, key="watch_time_score")
        }

    def run(self) -> None:
        if self._prepare_result:
            reels_items = self._prepare_result["reels_items"]
            self.context.reels_result = {
                "items": reels_items,
                "total_reels": len(reels_items),
                "avg_duration_ms": sum(r["duration_ms"] for r in reels_items)
                / max(len(reels_items), 1),
            }


class ExploreGridStep(FeedFlowStep):
    """
    Explore grid ranking and layout.
    Mimics IG's Explore flow with grid layout.
    """

    def enabled(self) -> bool:
        return (
            hasattr(self.context.request, "explore_mode")
            and self.context.request.explore_mode
        )

    def prepare(self) -> Dict[str, Any]:
        items = self.context.ranked_items[:20]

        # REMOVED: self._simulate_cpu_work(3) - reduces compute overhead
        explore_items = []
        for item in items:
            explore_item = copy.deepcopy(item)
            explore_item["media_type"] = random.choice(["image", "video", "carousel"])
            explore_item["explore_score"] = random.random() * 100
            explore_item["topic"] = random.choice(
                ["sports", "food", "travel", "fashion", "tech"]
            )

            explore_item["grid_position"] = {
                "row": len(explore_items) // 3,
                "col": len(explore_items) % 3,
            }

            explore_items.append(explore_item)

        return {
            "explore_items": self._simulate_ranking(explore_items, key="explore_score")
        }

    def run(self) -> None:
        if self._prepare_result:
            explore_items = self._prepare_result["explore_items"]

            topics = {}
            for item in explore_items:
                topic = item["topic"]
                topics[topic] = topics.get(topic, 0) + 1

            self.context.explore_result = {
                "items": explore_items,
                "total_items": len(explore_items),
                "topic_distribution": topics,
            }


class ChunkingStep(FeedFlowStep):
    """
    Chunked streaming for progressive loading.
    Mimics IG's StreamedChunkableTextFeedFlowSteps.
    """

    def enabled(self) -> bool:
        return (
            hasattr(self.context.request, "enable_chunking")
            and self.context.request.enable_chunking
        )

    def run(self) -> None:
        items = (
            self.context.feed_items
            if self.context.feed_items
            else self.context.ranked_items
        )

        chunk_size = 5
        chunks = []

        # REMOVED: self._simulate_cpu_work(2) - reduces compute overhead
        for i in range(0, len(items), chunk_size):
            chunk = items[i : i + chunk_size]
            chunk_metadata = {
                "chunk_index": len(chunks),
                "chunk_size": len(chunk),
                "items": chunk,
            }
            chunks.append(chunk_metadata)

        self.context.add_metric("chunks", chunks)
        self.context.add_metric("num_chunks", len(chunks))


class BrandSafetyStep(FeedFlowStep):
    """
    Brand safety and content filtering.
    Mimics IG's content safety and filtering steps.
    """

    def enabled(self) -> bool:
        return True

    def run(self) -> None:
        items = (
            self.context.feed_items
            if self.context.feed_items
            else self.context.ranked_items
        )

        # PRODUCTION-SCALE RPC STRATEGY: 3 RPC calls for brand safety
        # This dramatically amplifies Python↔Thrift boundary crossings!
        try:
            filter_client = get_filter_client()
            ranking_client = get_ranking_client()

            # Convert UUID to int for Thrift RPC calls
            user_id_int = (
                _uuid_to_int(self.context.user.id)
                if hasattr(self.context, "user")
                else 1
            )

            # RPC CALL #18: Content safety filtering (primary safety check)
            response = filter_client.filter_content(
                user_id=user_id_int,
                item_ids=[item["pk"] for item in items],
                filter_level="moderate",
            )

            # RPC CALL #19: Brand suitability check (additional safety layer)
            brand_check = filter_client.filter_content(
                user_id=user_id_int,
                item_ids=[item["pk"] for item in items],
                filter_level="strict",
            )

            # RPC CALL #20: Sensitive content detection (final safety validation)
            sensitive_check = ranking_client.rank_items(
                user_id=user_id_int,
                items=[item["pk"] for item in items],
                num_results=len(items),
            )

            # Filter items based on all RPC responses
            safe_item_ids = set(response.safe_item_ids) & set(brand_check.safe_item_ids)
            filtered_items = [item for item in items if item["pk"] in safe_item_ids]

            self.context.feed_items = filtered_items
            self.context.add_metric("items_filtered", len(items) - len(filtered_items))
        except Exception:
            # Fallback if filtering service unavailable
            filtered_items = self._simulate_filtering(items, filter_rate=0.1)
            self.context.feed_items = filtered_items
            self.context.add_metric("items_filtered", len(items) - len(filtered_items))


class ViewStateStep(FeedFlowStep):
    """
    Saves view state for pagination and deduplication.
    Mimics IG's view state management.
    """

    def enabled(self) -> bool:
        return True

    def run(self) -> None:
        items = (
            self.context.feed_items
            if self.context.feed_items
            else self.context.ranked_items
        )

        # REMOVED: self._simulate_cpu_work(1) - reduces compute overhead
        view_state = []
        for item in items:
            state_entry = {
                "media_id": item["pk"],
                "seen_at": random.random() * 1000000,
                "position": len(view_state),
            }
            view_state.append(state_entry)

        self.context.add_metric("view_state_size", len(view_state))
        self.context.add_metric("view_state", view_state)

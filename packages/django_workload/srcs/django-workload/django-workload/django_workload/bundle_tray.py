"""
Bundle Tray Handler for DjangoBench V2.

This module models the workload characteristics of feed.api.views.reels_tray
from production IG Django server.

The reels tray (stories tray) is the horizontal scrollable bar at the top of
the Instagram feed, showing profile bubbles for users with active Stories/Reels.

Key features modeled:
- Tray bucket construction with partial materialization
- User ranking via ML ranking pipelines
- Caching with ranked tray cache
- Special insertions (self, live, suggested users)
- User/Story metadata fetching via data access framework
- CPU-intensive primitives based on production leaf function profiles
"""

import logging
import random
import time
from typing import Any, Dict, List, Optional

from django.core.cache import cache

from .models import (
    BundleEntryModel,
    BundleReelClipModel,
    ClipVideoModel,
    FeedEntryModel,
    UserModel,
)
from .reels_tray import execute_random_primitives, MaterialTray, StoryTrayService

logger = logging.getLogger(__name__)


class BundleTray(object):
    """
    Handler for bundle_tray endpoint.

    Models the workload of feed.api.views.reels_tray from production IG.
    Constructs a tray of user buckets with stories/reels, supporting:
    - Partial materialization (first N filled, rest skeletons)
    - User ranking and sourcing via StoryTrayService
    - Caching and metadata fetching
    - CPU-intensive primitives based on production leaf function profiles
    """

    # Configuration constants matching production behavior
    NUM_FILLED_BUCKETS = 4  # First N buckets fully materialized
    MAX_ITEMS_PER_BUCKET = 10  # Max stories/reels per user
    CACHE_TTL = 300  # 5 minutes
    NUM_CPU_PRIMITIVES = 15  # Number of CPU primitives to execute per request

    def __init__(self, request):
        self.request = request
        self.user = request.user
        # Initialize the StoryTrayService for Thrift RPC operations
        self.story_tray_service: Optional[StoryTrayService] = None

    def _get_story_tray_service(self) -> StoryTrayService:
        """Lazily initialize StoryTrayService for tray operations."""
        if self.story_tray_service is None:
            self.story_tray_service = StoryTrayService(self.request, self.user)
        return self.story_tray_service

    def get_bundle(self) -> Dict[str, Any]:
        """
        Main entry point for tray construction.

        Models the full reels_tray request flow:
        1. Check cache for prefetched results
        2. Execute CPU-intensive primitives (ML pipeline, experiment, feature flag, etc.)
        3. Source candidate users with active stories
        4. Rank candidates via StoryTrayService (Thrift RPC)
        5. Fetch user metadata via StoryTrayService (Thrift RPC)
        6. Build buckets with partial materialization
        7. Add reel clips to each bucket
        8. Cache results for future requests

        Returns:
            Dict with tray buckets and metadata
        """
        start_time = time.time()

        # Step 1: Check cache for prefetched results
        cache_key = self._get_cache_key()
        cached_result = cache.get(cache_key)
        if cached_result is not None:
            logger.debug(f"[perf] bundle_tray::cache_hit: {time.time() - start_time}")
            return cached_result

        # Step 2: Execute CPU-intensive primitives (models production leaf functions)
        # This simulates the CPU work from:
        # - ML pipeline response building (18.25%)
        # - Experiment evaluation (9.13%)
        # - Feature flag evaluation (10.12%)
        # - Config resolution, metrics, caching (7.24%)
        primitive_results = execute_random_primitives(
            num_executions=self.NUM_CPU_PRIMITIVES
        )
        logger.debug(
            f"[perf] bundle_tray::cpu_primitives: {time.time() - start_time}, "
            f"executed={len(primitive_results)}"
        )

        # Step 3: Source candidate bundles (users with active stories)
        bundles = self._source_candidate_bundles()
        logger.debug(
            f"[perf] bundle_tray::source_candidates: {time.time() - start_time}"
        )

        # Step 4: Deduplicate - only one bundle per user
        userids, feedentryids, first_bundleids = self._deduplicate_bundles(bundles)

        # Step 5: Fetch user information with caching (via StoryTrayService if available)
        userinfo = self._fetch_user_info(userids)
        logger.debug(f"[perf] bundle_tray::fetch_user_info: {time.time() - start_time}")

        # Step 6: Fetch feed entry information with caching
        feedentryinfo = self._fetch_feed_entry_info(feedentryids)

        # Step 7: Fetch reel clips for each bundle
        reel_clips_by_bundle = self._fetch_reel_clips_for_bundles(
            [b for b in bundles if b.id in first_bundleids]
        )
        logger.debug(
            f"[perf] bundle_tray::fetch_reel_clips: {time.time() - start_time}"
        )

        # Step 8: Build tray buckets with partial materialization
        result = self._build_tray_buckets(
            bundles,
            first_bundleids,
            userinfo,
            feedentryinfo,
            reel_clips_by_bundle,
        )

        # Step 9: Cache results
        cache.set(cache_key, result, self.CACHE_TTL)

        logger.debug(
            f"[perf] bundle_tray::total_get_bundle: {time.time() - start_time}"
        )
        return result

    def get_bundle_via_service(self) -> Dict[str, Any]:
        """
        Alternative entry point using StoryTrayService for full tray construction.

        This method delegates the entire tray construction to StoryTrayService,
        which handles:
        - Thrift RPC calls for ranking and metadata
        - Partial materialization logic
        - Caching via ranked tray cache

        Returns:
            Dict with tray data from MaterialTray response
        """
        start_time = time.time()

        # Execute CPU-intensive primitives before service call
        execute_random_primitives(num_executions=self.NUM_CPU_PRIMITIVES)
        logger.debug(
            f"[perf] bundle_tray::cpu_primitives_service: {time.time() - start_time}"
        )

        # Use StoryTrayService for tray construction
        service = self._get_story_tray_service()
        material_tray: MaterialTray = service.get_tray()

        logger.debug(
            f"[perf] bundle_tray::service_get_tray: {time.time() - start_time}"
        )

        return material_tray.to_dict()

    def _get_cache_key(self) -> str:
        """Generate cache key for tray results."""
        return f"ranked_tray:{self.user.id}:head"

    def _source_candidate_bundles(self) -> List[BundleEntryModel]:
        """Source candidate users who have active stories/reels."""
        following = self.user.following or []
        return list(
            BundleEntryModel.objects.filter(userid__in=following).limit(
                20
            )  # Increased limit for better coverage
        )

    def _deduplicate_bundles(self, bundles):
        """Deduplicate bundles - only one per user."""
        userids = {}
        feedentryids = []
        for bundle in bundles:
            if bundle.userid in userids:
                continue
            userids[bundle.userid] = bundle.id
            feedentryids += bundle.entry_ids
        first_bundleids = set(userids.values())
        return userids, feedentryids, first_bundleids

    def _fetch_user_info(self, userids: Dict) -> Dict:
        """Fetch user information with caching (models data access framework pattern)."""
        userinfo = cache.get_many(list(userids))
        if userinfo is not None:
            missing_userinfo = [uid for uid in userids if uid not in userinfo]
            if missing_userinfo:
                for user in UserModel.objects.filter(id__in=missing_userinfo):
                    userinfo[user.id] = user.json_data
                cache.set_many(
                    {uid: userinfo[uid] for uid in missing_userinfo}, self.CACHE_TTL
                )
        else:
            userinfo = {}
            for user in UserModel.objects.filter(id__in=list(userids)):
                userinfo[user.id] = user.json_data
            cache.set_many(userinfo, self.CACHE_TTL)
        return userinfo

    def _fetch_feed_entry_info(self, feedentryids: List) -> Dict:
        """Fetch feed entry information with caching."""
        feedentryinfo = cache.get_many(list(feedentryids))
        if feedentryinfo is not None:
            missing_feedentryinfo = [
                fid for fid in feedentryids if fid not in feedentryinfo
            ]
            if missing_feedentryinfo:
                for feedentry in FeedEntryModel.objects.filter(
                    id__in=missing_feedentryinfo
                ):
                    feedentryinfo[feedentry.id] = {
                        "pk": str(feedentry.id),
                        "comment_count": feedentry.comment_count,
                        "published": feedentry.published.timestamp(),
                    }
                cache.set_many(
                    {fid: feedentryinfo[fid] for fid in missing_feedentryinfo}
                )
        else:
            feedentryinfo = {}
            for feedentry in FeedEntryModel.objects.filter(id__in=list(feedentryids)):
                feedentryinfo[feedentry.id] = {
                    "pk": str(feedentry.id),
                    "comment_count": feedentry.comment_count,
                    "published": feedentry.published.timestamp(),
                }
            cache.set_many(feedentryinfo, self.CACHE_TTL)
        return feedentryinfo

    def _fetch_reel_clips_for_bundles(
        self, bundles: List[BundleEntryModel]
    ) -> Dict[str, List[Dict[str, Any]]]:
        """
        Fetch reel clips associated with each bundle.

        Models fetching media items for stories/reels from inventory.
        Only fetches for first N bundles (filled buckets).
        """
        reel_clips_by_bundle: Dict[str, List[Dict[str, Any]]] = {}

        for _idx, bundle in enumerate(bundles[: self.NUM_FILLED_BUCKETS]):
            bundle_id = str(bundle.id)
            reel_clips_by_bundle[bundle_id] = []

            try:
                # Query BundleReelClipModel for clips associated with this bundle
                bundle_clips = list(
                    BundleReelClipModel.objects.filter(bundle_id=bundle.id).limit(
                        self.MAX_ITEMS_PER_BUCKET
                    )
                )

                # Fetch full clip details
                clip_ids = [bc.clip_id for bc in bundle_clips]
                if clip_ids:
                    clips = {
                        c.id: c for c in ClipVideoModel.objects.filter(id__in=clip_ids)
                    }

                    for bc in bundle_clips:
                        clip = clips.get(bc.clip_id)
                        if clip:
                            reel_clips_by_bundle[bundle_id].append(
                                {
                                    "pk": str(clip.id),
                                    "media_type": "VIDEO",
                                    "duration_ms": clip.duration_ms,
                                    "thumbnail_url": clip.thumbnail_url,
                                    "title": clip.title,
                                    "view_count": clip.view_count,
                                    "position": bc.position,
                                }
                            )

            except Exception as e:
                logger.debug(f"No reel clips found for bundle {bundle_id}: {e}")

        return reel_clips_by_bundle

    def _build_tray_buckets(
        self,
        bundles: List[BundleEntryModel],
        first_bundleids: set,
        userinfo: Dict,
        feedentryinfo: Dict,
        reel_clips_by_bundle: Dict[str, List[Dict[str, Any]]],
    ) -> Dict[str, Any]:
        """
        Build tray buckets with partial materialization.

        First N buckets are fully filled (materialized) with media data.
        Remaining buckets are skeletons (minimal info, no media).
        """
        tray_items = []
        bucket_index = 0

        for b in bundles:
            if b.id not in first_bundleids:
                continue

            is_filled = bucket_index < self.NUM_FILLED_BUCKETS
            bundle_id = str(b.id)

            # Build bucket
            bucket = {
                "pk": bundle_id,
                "comment_count": b.comment_count,
                "published": b.published.timestamp(),
                "user": userinfo.get(b.userid, {"pk": str(b.userid)}),
                "is_filled": is_filled,
                "ranking_score": random.random(),  # Simulated ranking score
            }

            if is_filled:
                # Filled bucket: include feed entries and reel clips
                bucket["items"] = [
                    feedentryinfo[f] for f in b.entry_ids if f in feedentryinfo
                ]
                bucket["reel_clips"] = reel_clips_by_bundle.get(bundle_id, [])
            else:
                # Skeleton bucket: minimal info
                bucket["items"] = []
                bucket["reel_clips"] = []

            tray_items.append(bucket)
            bucket_index += 1

        return {
            "tray": tray_items,
            "paging_info": {
                "max_id": tray_items[-1]["pk"] if tray_items else None,
                "more_available": len(tray_items) >= 10,
            },
            "status": "ok",
        }

    def get_bundle_legacy(self) -> Dict[str, Any]:
        """
        Legacy get_bundle implementation for backward compatibility.

        Returns old-style bundle format without reel clips.
        """
        start_time = time.time()

        bundles = list(
            BundleEntryModel.objects.filter(
                userid__in=self.request.user.following
            ).limit(10)
        )
        logger.debug(
            "[perf] bundle_tray::bundle_entry.objects.filter: {}".format(
                time.time() - start_time
            )
        )

        # only one bundle per user
        userids = {}
        feedentryids = []
        for bundle in bundles:
            if bundle.userid in userids:
                continue
            userids[bundle.userid] = bundle.id
            feedentryids += bundle.entry_ids
        first_bundleids = set(userids.values())

        # Fetch user information
        start_time = time.time()
        userinfo = cache.get_many(list(userids))
        if userinfo is not None:
            missing_userinfo = [userid for userid in userids if userid not in userinfo]
            if missing_userinfo:
                for user in UserModel.objects.filter(id__in=missing_userinfo):
                    userinfo[user.id] = user.json_data

                cache.set_many({uid: userinfo[uid] for uid in missing_userinfo}, 60 * 5)
        else:
            userinfo = {}
            for user in UserModel.objects.filter(id__in=list(userids)):
                userinfo[user.id] = user.json_data
            cache.set_many(userinfo, 60 * 5)
        logger.debug(
            "[perf] bundle_tray::user_model.objects.filter: {}".format(
                time.time() - start_time
            )
        )

        # fetch entry information
        feedentryinfo = {}
        start_time = time.time()
        feedentryinfo = cache.get_many(list(feedentryids))
        if feedentryinfo is not None:
            missing_feedentryinfo = [
                fid for fid in feedentryids if fid not in feedentryinfo
            ]
            if missing_feedentryinfo:
                for feedentry in FeedEntryModel.objects.filter(
                    id__in=missing_feedentryinfo
                ):
                    feedentryinfo[feedentry.id] = {
                        "pk": str(feedentry.id),
                        "comment_count": feedentry.comment_count,
                        "published": feedentry.published.timestamp(),
                    }
                cache.set_many(
                    {fid: feedentryinfo[fid] for fid in missing_feedentryinfo}
                )
        else:
            for feedentry in FeedEntryModel.objects.filter(id__in=list(feedentryids)):
                feedentryinfo[feedentry.id] = {
                    "pk": str(feedentry.id),
                    "comment_count": feedentry.comment_count,
                    "published": feedentry.published.timestamp(),
                }
            cache.set_many(feedentryinfo, 60 * 5)

        result = {
            "bundle": [
                {
                    "pk": str(b.id),
                    "comment_count": b.comment_count,
                    "published": b.published.timestamp(),
                    "user": userinfo.get(b.userid, {}),
                    "items": [
                        feedentryinfo[f] for f in b.entry_ids if f in feedentryinfo
                    ],
                }
                for b in bundles
                if b.id in first_bundleids
            ]
        }
        logger.debug(
            "[perf] bundle_tray::feed_entry.objects.filter+bundle_process: {}".format(
                time.time() - start_time
            )
        )
        return result

    def dup_sort_data(self, bundle_list, conf):
        """Duplicate and sort data for CPU-intensive processing."""
        for _ in range(conf.get_mult_factor()):
            conf.list_extend(bundle_list)
        sorted_list = sorted(
            conf.get_list(), key=lambda x: x["published"], reverse=True
        )
        conf.final_items = []
        return sorted_list

    def undup_data(self, item, conf):
        """Remove duplicate items."""
        exists = False
        for final_item in conf.final_items:
            if final_item["published"] == item["published"]:
                exists = True
                break
        if not exists:
            conf.final_items.append(item)

    def post_process(self, res):
        """
        Post-process bundle results.

        Applies deduplication and comment counting for CPU-intensive work.
        """
        # Handle new format (tray) or old format (bundle)
        if "tray" in res:
            bundle_list = res["tray"]
        else:
            bundle_list = res.get("bundle", [])

        conf = BundleConfig()

        sorted_list = self.dup_sort_data(bundle_list, conf)
        for item in sorted_list:
            conf.comm_total = conf.comm_total + item.get("comment_count", 0)
            for sub in item.get("items", []):
                conf.comm_total = conf.comm_total + sub.get("comment_count", 0)
            # un-duplicate the data
            self.undup_data(item, conf)

        res["comments_total"] = int(conf.comm_total / conf.get_mult_factor())

        if "tray" in res:
            res["tray"] = conf.final_items
        else:
            res["bundle"] = conf.final_items
        return res


class BundleConfig(object):
    """Configuration for bundle processing."""

    def __init__(self):
        # Number of times the original bundle list is duplicated in order
        # to make the view more Python intensive
        self.mult_factor = 1
        self.comm_total = 0
        self.work_list = []
        self.final_items = []

    def get_mult_factor(self):
        return self.mult_factor

    def list_extend(self, list_):
        self.work_list.extend(list_)

    def get_list(self):
        return self.work_list

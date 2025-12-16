# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
FeedFlowContext - Shared state container for FeedFlow steps
"""

from typing import Any, Dict, List, Optional


class FeedFlowContext:
    """
    Shared context object passed between all FeedFlow steps.
    Mimics the IG Django FeedFlowContext pattern where steps read
    and mutate shared state.
    """

    def __init__(self, request, user):
        # Input data (immutable)
        self.request = request
        self.user = user
        self.session_id = getattr(request, "session_id", None)

        # Mutable state (modified by steps)
        self.feed_items: List[Dict[str, Any]] = []
        self.ads_items: List[Dict[str, Any]] = []
        self.ranked_items: List[Dict[str, Any]] = []
        self.timeline_response: Optional[Dict[str, Any]] = None

        # Step-specific results
        self.source_and_rank_result: Optional[Any] = None
        self.ads_context: Optional[Dict[str, Any]] = None
        self.text_feed_result: Optional[Any] = None
        self.reels_result: Optional[Any] = None
        self.explore_result: Optional[Any] = None

        # Configuration
        self.allow_ads = True
        self.enable_ranking = True
        self.page_size = 20

        # Metrics
        self.metrics: Dict[str, Any] = {
            "steps_executed": [],
            "step_timings": {},
        }

    def add_metric(self, key: str, value: Any) -> None:
        """Add a metric to the context"""
        self.metrics[key] = value

    def record_step_execution(self, step_name: str, duration_ms: float) -> None:
        """Record step execution for metrics"""
        self.metrics["steps_executed"].append(step_name)
        self.metrics["step_timings"][step_name] = duration_ms

# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
FeedFlow - Main orchestration class
Mimics IG Django's FeedFlow with multi-step pipeline execution
"""

import random
import time
from typing import Any, Dict, List

from .context import FeedFlowContext
from .step import FeedFlowStep
from .steps import (
    BrandSafetyStep,
    ChunkingStep,
    ExploreGridStep,
    FetchAdsStep,
    InsertAdsStep,
    ReelsFeedFlowStep,
    SourceAndRankStep,
    TextFeedFlowStep,
    TimelineStep,
    ViewStateStep,
)


class FeedFlow:
    """
    Main FeedFlow orchestrator.
    Mimics IG Django's FeedFlow with before_run, run, and after_run phases.
    """

    def __init__(self, request):
        self.request = request
        self.context = FeedFlowContext(request, request.user)
        self.custom_steps: List[FeedFlowStep] = []  # For variant view functions

    def add_step(self, step: FeedFlowStep) -> None:
        """
        Add a custom step to the flow.
        Used by variant view functions to inject specific step combinations.
        """
        step.context = self.context
        self.custom_steps.append(step)

    def _get_flow_steps(self) -> List[FeedFlowStep]:
        """
        Returns list of FeedFlow steps to execute.
        Randomly selects steps to simulate different flow variants and maximize I-cache misses.
        """
        # Core steps that run most of the time
        core_steps = [
            SourceAndRankStep(self.context),
            TimelineStep(self.context),
        ]

        # Optional steps with varying probabilities
        optional_steps = [
            (FetchAdsStep(self.context), 0.7),  # 70% chance
            (InsertAdsStep(self.context), 0.7),  # 70% chance
            (BrandSafetyStep(self.context), 0.6),  # 60% chance
            (ViewStateStep(self.context), 0.5),  # 50% chance
            (TextFeedFlowStep(self.context), 0.3),  # 30% chance
            (ReelsFeedFlowStep(self.context), 0.3),  # 30% chance
            (ExploreGridStep(self.context), 0.3),  # 30% chance
            (ChunkingStep(self.context), 0.2),  # 20% chance
        ]

        # Randomly select optional steps based on probabilities
        selected_steps = core_steps.copy()
        for step, probability in optional_steps:
            if random.random() < probability:
                selected_steps.append(step)

        # Shuffle to vary execution order (increases I-cache misses)
        random.shuffle(selected_steps)

        # Ensure TimelineStep runs last for response generation
        timeline_steps = [s for s in selected_steps if isinstance(s, TimelineStep)]
        other_steps = [s for s in selected_steps if not isinstance(s, TimelineStep)]
        selected_steps = other_steps + timeline_steps

        return selected_steps

    def _before_run(self) -> None:
        """
        Before run phase - context initialization.
        Mimics IG's _async_before_run() with parallel IO gathering.
        """
        pass

    def _run(self) -> None:
        """
        Main run phase - executes all flow steps in sequence.
        Mimics IG's _async_run() with step registry and execution.
        """
        # Use custom steps if they were added, otherwise use default flow
        if self.custom_steps:
            steps = self.custom_steps
        else:
            steps = self._get_flow_steps()

        for step in steps:
            step.execute()

    def _after_run(self) -> None:
        """
        After run phase - finalization and logging.
        Mimics IG's _async_after_run() with view state persistence and logging.
        """
        pass

    def next_page(self) -> Dict[str, Any]:
        """
        Main entry point for FeedFlow execution.
        Mimics IG's next_page() with three-phase execution.
        Returns timeline response.
        """
        start_time = time.time()

        self._before_run()

        self._run()

        self._after_run()

        total_duration_ms = (time.time() - start_time) * 1000

        if self.context.timeline_response:
            response = self.context.timeline_response.copy()
        else:
            response = {"num_results": 0, "items": [], "feed_type": "unknown"}

        response["metrics"] = {
            "total_duration_ms": total_duration_ms,
            "steps_executed": self.context.metrics["steps_executed"],
            "step_timings": self.context.metrics["step_timings"],
        }

        if self.context.text_feed_result:
            response["text_feed"] = self.context.text_feed_result

        if self.context.reels_result:
            response["reels"] = self.context.reels_result

        if self.context.explore_result:
            response["explore"] = self.context.explore_result

        return response

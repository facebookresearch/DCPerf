# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
FeedFlowStep - Base class for FeedFlow steps
Mimics the IG Django FeedFlowStep pattern with prepare/run phases
"""

import json
import random
import time
from abc import ABC, abstractmethod
from typing import Any, Dict, List, Optional

from .context import FeedFlowContext
from .primitives import execute_random_primitives


class FeedFlowStep(ABC):
    """
    Base class for all FeedFlow steps.
    Each step implements:
    - enabled(): Check if step should run
    - prepare(): Optional preparation (CPU-intensive, read-heavy)
    - run(): Main execution (context mutation, write-heavy)
    """

    def __init__(self, context: FeedFlowContext) -> None:
        self.context = context
        self._prepare_result: Optional[Any] = None
        self._is_enabled = False

    @abstractmethod
    def enabled(self) -> bool:
        """Return True if this step should execute"""
        pass

    def prepare(self) -> Optional[Any]:
        """
        Preparation phase - CPU-intensive operations, read-heavy work.
        This mimics async_prepare() in IG Django.
        Can return data to be used in run().
        """
        return None

    @abstractmethod
    def run(self) -> None:
        """
        Main execution phase - mutates context, write-heavy work.
        This mimics async_run() in IG Django.
        """
        pass

    def execute(self) -> None:
        """
        Full execution wrapper - handles enabled check, prepare, and run phases.
        This mimics the automatic wrapper in IG Django's FeedFlowStep.
        """
        step_name = self.__class__.__name__
        start_time = time.time()

        self._is_enabled = self.enabled()

        if not self._is_enabled:
            return

        self._prepare_result = self.prepare()

        self.run()

        duration_ms = (time.time() - start_time) * 1000
        self.context.record_step_execution(step_name, duration_ms)

    def _simulate_cpu_work(self, num_primitives: int = 3) -> None:
        """
        Simulate CPU-intensive work by randomly executing diverse primitives.

        This maximizes I-cache misses by jumping between different code paths
        in the Python interpreter rather than running the same operations repeatedly.

        Args:
            num_primitives: Number of random primitives to execute (default: 3)
        """
        execute_random_primitives(num_primitives)
        return None

    def _simulate_ranking(
        self, items: List[Dict[str, Any]], key: str = "score"
    ) -> List[Dict[str, Any]]:
        """Simulate ranking logic with CPU work"""
        for item in items:
            if key not in item:
                item[key] = random.random() * 100
                self._simulate_cpu_work(2)

        return sorted(items, key=lambda x: x[key], reverse=True)

    def _simulate_serialization(self, data: Any) -> str:
        """Simulate serialization work"""
        self._simulate_cpu_work(1)
        return json.dumps(data)

    def _simulate_filtering(
        self, items: List[Dict[str, Any]], filter_rate: float = 0.2
    ) -> List[Dict[str, Any]]:
        """Simulate filtering logic"""
        result = []
        for item in items:
            self._simulate_cpu_work(1)
            if random.random() > filter_rate:
                result.append(item)
        return result

# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
Reels Tray module for DjangoBench V2.

Provides StoryTrayService for modeling the workload of feed.api.views.reels_tray
from production IG Django server. This module handles:
- Tray construction for Stories/Reels at the top of the feed
- User bucketing and ranking via ML ranking pipelines
- Caching with ranked tray cache
- Partial materialization (first N trays filled, rest are skeletons)
"""

# Primitives
from .primitives import (
    execute_random_primitives,
    get_primitive_methods,
    PRIMITIVE_WEIGHTS,
    ReelsTrayPrimitives,
)

# Base service classes
from .service import (
    MaterialTray,
    ReelBucket,
    ReelsTrayContext,
    ReelsTrayRequest,
    StoryTrayService,
)

# Thrift clients
from .thrift_client import get_tray_ranking_client, get_user_metadata_client

# All exports
__all__ = [
    "execute_random_primitives",
    "get_primitive_methods",
    "get_tray_ranking_client",
    "get_user_metadata_client",
    "MaterialTray",
    "PRIMITIVE_WEIGHTS",
    "ReelBucket",
    "ReelsTrayContext",
    "ReelsTrayPrimitives",
    "ReelsTrayRequest",
    "StoryTrayService",
]

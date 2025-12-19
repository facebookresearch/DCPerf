# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
Clips Discovery module for DjangoBench V2.

Provides ClipsDiscoverService variants for clips/reels discovery
with weighted CPU primitives for realistic workload simulation.
"""

# Primitives
from .primitives import (
    ClipsDiscoveryPrimitives,
    execute_random_primitives,
    get_primitive_methods,
    PRIMITIVE_WEIGHTS,
)

# Base service classes
from .service import (
    ClipsDiscoverContext,
    ClipsDiscoverRequest,
    ClipsDiscoverResponse,
    ClipsDiscoverService,
    ClipsDiscoverStreamingService,
)

# Thrift clients
from .thrift_client import get_clips_ads_client, get_clips_ranking_client

# All exports
__all__ = [
    "ClipsDiscoverContext",
    "ClipsDiscoverRequest",
    "ClipsDiscoverResponse",
    "ClipsDiscoverService",
    "ClipsDiscoverStreamingService",
    "ClipsDiscoveryPrimitives",
    "PRIMITIVE_WEIGHTS",
    "execute_random_primitives",
    "get_primitive_methods",
    "get_clips_ads_client",
    "get_clips_ranking_client",
]

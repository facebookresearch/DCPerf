# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
Clips Discovery module for DjangoBench V2.

This module models the workload of clips.api.views.async_stream_clips_discover
from production IG Django server.

The main components are:
- ClipsDiscoverService: Main service class for clips discovery
- ClipsDiscoverStreamingService: Streaming variant for chunked delivery
- thrift_client: Thrift RPC client for fetching clips ads
"""

from .service import ClipsDiscoverService, ClipsDiscoverStreamingService

__all__ = ["ClipsDiscoverService", "ClipsDiscoverStreamingService"]

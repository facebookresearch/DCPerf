# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
Thrift RPC Client for Clips Ads Service.

This module provides Thrift clients for fetching clips ads, similar to
the AsyncAdsFetcherV2 used in production IG's clips discovery flow.

Uses connection pooling to reuse connections instead of creating new
sockets for every RPC call.
"""

import logging
import os
import sys
import threading
from pathlib import Path
from typing import Any, Dict, List

# Add OSS fbthrift Python library to path
FBTHRIFT_PREFIX = os.environ.get(
    "FBTHRIFT_PREFIX", "/home/wsu/proxygen/proxygen/_build/deps"
)
FBTHRIFT_PY_PATH = Path(FBTHRIFT_PREFIX) / "lib" / "fb-py-libs" / "thrift_py"
if FBTHRIFT_PY_PATH.exists():
    sys.path.insert(0, str(FBTHRIFT_PY_PATH))

# Add generated Thrift bindings to path
THRIFT_DIR = Path(__file__).parent.parent / "thrift"
GEN_PY_PATH = THRIFT_DIR / "build" / "gen-py3"
sys.path.insert(0, str(GEN_PY_PATH))

# Import generated Thrift types from py:asyncio generator
from mock_services import ttypes

# Import generated Client classes
from mock_services.MockAdsService import Client as MockAdsServiceClient
from mock_services.MockRankingService import Client as MockRankingServiceClient

# Import Thrift transport and protocol classes
from thrift.protocol import TBinaryProtocol
from thrift.transport import TSocket, TTransport

logger = logging.getLogger(__name__)


def _get_thrift_server_config() -> tuple:
    """
    Get Thrift server host and port from Django settings.

    Uses HAProxy on port 9090 which load-balances to backend servers.

    Returns:
        (host, port) tuple for Thrift server connection
    """
    try:
        from django.conf import settings

        host = getattr(settings, "THRIFT_SERVER_HOST", "localhost")
        port = getattr(settings, "THRIFT_SERVER_PORT", 9090)
        return host, port
    except Exception:
        return "localhost", 9090


class ThriftConnectionPool:
    """
    Thread-safe connection pool for Thrift clients.

    Maintains persistent connections and reuses them instead of
    creating new sockets for every RPC call.
    """

    def __init__(self, host: str, port: int, pool_size: int = 10):
        self.host = host
        self.port = port
        self.pool_size = pool_size
        self._pool = []
        self._lock = threading.Lock()
        logger.debug(
            f"ThriftConnectionPool initialized - "
            f"host={host}, port={port}, pool_size={pool_size}"
        )

    def _create_connection(self):
        """Create a new Thrift connection."""
        transport = TSocket.TSocket(self.host, self.port)
        transport = TTransport.TBufferedTransport(transport)
        protocol = TBinaryProtocol.TBinaryProtocol(transport)
        return transport, protocol

    def get_connection(self):
        """Get a connection from the pool or create a new one."""
        with self._lock:
            if self._pool:
                transport, protocol = self._pool.pop()
                if transport.isOpen():
                    logger.debug("Reusing connection from pool")
                    return transport, protocol
                else:
                    logger.debug("Connection in pool is closed, creating new one")

            logger.debug("Creating new connection")
            transport, protocol = self._create_connection()
            try:
                transport.open()
            except Exception as e:
                logger.error(f"Failed to open connection: {e}")
                raise
            return transport, protocol

    def return_connection(self, transport, protocol):
        """Return a connection to the pool for reuse."""
        with self._lock:
            if len(self._pool) < self.pool_size and transport.isOpen():
                logger.debug("Returning connection to pool")
                self._pool.append((transport, protocol))
            else:
                logger.debug("Pool full or connection closed, closing transport")
                try:
                    transport.close()
                except Exception:
                    pass

    def close_all(self):
        """Close all connections in the pool."""
        with self._lock:
            for transport, _ in self._pool:
                try:
                    transport.close()
                except Exception:
                    pass
            self._pool.clear()
            logger.debug("All connections closed")


class ClipsAdsData:
    """
    Simple wrapper for clips ad data extracted from Thrift response.
    Used to pass ad data to clips discovery service.
    """

    def __init__(self, thrift_ad):
        """Initialize from Thrift AdInsertion object."""
        self.ad_id = thrift_ad.ad_id
        self.campaign_id = thrift_ad.campaign_id
        self.creative_id = thrift_ad.creative_id
        self.advertiser_id = thrift_ad.advertiser_id
        self.tracking_token = thrift_ad.tracking_token
        self.impression_id = thrift_ad.impression_id
        self.ad_title = thrift_ad.ad_title
        self.ad_subtitle = thrift_ad.ad_subtitle
        self.call_to_action = thrift_ad.call_to_action
        self.destination_url = thrift_ad.destination_url
        self.comment_count = thrift_ad.comment_count
        self.quality_score = thrift_ad.quality_score
        self.predicted_ctr = thrift_ad.predicted_ctr
        self.predicted_cvr = thrift_ad.predicted_cvr
        self.is_video = thrift_ad.is_video
        self.video_duration = thrift_ad.video_duration

    def to_clips_item_dict(self) -> Dict[str, Any]:
        """Convert to ClipsItemDictWithAds format for clips discovery."""
        return {
            "pk": f"ad_{self.ad_id}",
            "media": {
                "pk": f"ad_media_{self.ad_id}",
                "media_type": "VIDEO" if self.is_video else "PHOTO",
                "video_duration": self.video_duration,
            },
            "ad_media": {
                "ad_id": self.ad_id,
                "campaign_id": self.campaign_id,
                "creative_id": self.creative_id,
                "advertiser_id": self.advertiser_id,
                "tracking_token": self.tracking_token,
                "impression_id": self.impression_id,
                "ad_title": self.ad_title,
                "ad_subtitle": self.ad_subtitle,
                "call_to_action": self.call_to_action,
                "destination_url": self.destination_url,
            },
            "netego_media": None,
            "is_ad": True,
            "ad_score": self.quality_score,
            "predicted_ctr": self.predicted_ctr,
            "predicted_cvr": self.predicted_cvr,
            "user": {
                "name": f"Advertiser_{self.advertiser_id}",
                "pk": f"advertiser_{self.advertiser_id}",
            },
            "comment_count": self.comment_count,
        }


class ThriftClipsAdsClient:
    """
    Thrift RPC client for Clips Ads Service with connection pooling.

    Fetches ads for blending into clips discovery results.
    Mimics AsyncAdsFetcherV2 from production IG Django.
    """

    _connection_pool = None
    _pool_lock = threading.Lock()

    def __init__(self, host: str = "localhost", port: int = 9090):
        """Initialize Thrift client with connection pooling."""
        self.host = host
        self.port = port

        with ThriftClipsAdsClient._pool_lock:
            if ThriftClipsAdsClient._connection_pool is None:
                ThriftClipsAdsClient._connection_pool = ThriftConnectionPool(
                    host=host, port=port, pool_size=20
                )

        logger.debug(f"ThriftClipsAdsClient initialized - host={host}, port={port}")

    def fetch_clips_ads(
        self, user_id: int, num_ads: int, surface_type: str = "CLIPS"
    ) -> List[ClipsAdsData]:
        """
        Fetch ads for clips discovery via REAL Thrift RPC call.

        Args:
            user_id: User ID for personalized ads
            num_ads: Number of ads to fetch
            surface_type: Surface type (CLIPS, REELS, etc.)

        Returns:
            List of ClipsAdsData objects
        """
        transport = None
        protocol = None

        try:
            transport, protocol = ThriftClipsAdsClient._connection_pool.get_connection()
            client = MockAdsServiceClient(protocol)

            request = ttypes.FetchAdsRequest(
                user_id=user_id,
                num_ads_requested=num_ads,
                surface_type=surface_type,
                context={"surface": "clips_discover"},
            )

            response = client.fetchAds(request)
            logger.debug(f"Fetched {len(response.ads)} clips ads for user {user_id}")

            ThriftClipsAdsClient._connection_pool.return_connection(transport, protocol)

            ads = []
            for ad_thrift in response.ads:
                ads.append(ClipsAdsData(ad_thrift))

            return ads

        except Exception as e:
            logger.error(f"Thrift RPC error in fetch_clips_ads: {e}")
            if transport:
                try:
                    transport.close()
                except Exception:
                    pass
            return []


class ThriftClipsRankingClient:
    """
    Thrift RPC client for Clips Ranking Service with connection pooling.

    Ranks clips items for personalized discovery.
    """

    _connection_pool = None
    _pool_lock = threading.Lock()

    def __init__(self, host: str = "localhost", port: int = 9090):
        """Initialize Thrift client with connection pooling."""
        self.host = host
        self.port = port

        with ThriftClipsRankingClient._pool_lock:
            if ThriftClipsRankingClient._connection_pool is None:
                ThriftClipsRankingClient._connection_pool = ThriftConnectionPool(
                    host=host, port=port, pool_size=20
                )

        logger.debug(f"ThriftClipsRankingClient initialized - host={host}, port={port}")

    def rank_clips(
        self, user_id: int, clip_ids: List[str], num_results: int
    ) -> Dict[str, float]:
        """
        Rank clips via REAL Thrift RPC call.

        Args:
            user_id: User ID for personalized ranking
            clip_ids: List of clip IDs to rank
            num_results: Number of results to return

        Returns:
            Dict mapping clip_id to score
        """
        transport = None
        protocol = None

        try:
            transport, protocol = (
                ThriftClipsRankingClient._connection_pool.get_connection()
            )
            client = MockRankingServiceClient(protocol)

            request = ttypes.RankItemsRequest(
                user_id=user_id,
                item_ids=clip_ids,
                num_results=num_results,
            )

            response = client.rankItems(request)
            logger.debug(f"Ranked {len(response.item_ids)} clips for user {user_id}")

            ThriftClipsRankingClient._connection_pool.return_connection(
                transport, protocol
            )

            score_map = {
                item_id: score
                for item_id, score in zip(response.item_ids, response.scores)
            }
            return score_map

        except Exception as e:
            logger.error(f"Thrift RPC error in rank_clips: {e}")
            if transport:
                try:
                    transport.close()
                except Exception:
                    pass
            return {}


# Global client instances
_clips_ads_client_instance = None
_clips_ranking_client_instance = None


def get_clips_ads_client() -> ThriftClipsAdsClient:
    """Get or create global Thrift clips ads client instance."""
    global _clips_ads_client_instance
    if _clips_ads_client_instance is None:
        host, port = _get_thrift_server_config()
        _clips_ads_client_instance = ThriftClipsAdsClient(host=host, port=port)
    return _clips_ads_client_instance


def get_clips_ranking_client() -> ThriftClipsRankingClient:
    """Get or create global Thrift clips ranking client instance."""
    global _clips_ranking_client_instance
    if _clips_ranking_client_instance is None:
        host, port = _get_thrift_server_config()
        _clips_ranking_client_instance = ThriftClipsRankingClient(host=host, port=port)
    return _clips_ranking_client_instance

# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
Thrift RPC Client using py:asyncio generator (OSS-compatible).

This module provides Thrift clients that make REAL RPC calls to create
Python↔Thrift boundary crossings for I-cache pressure.

Uses py:asyncio generator (pure Python async bindings).
Since Django is synchronous but Thrift clients are async-capable, we use
traditional synchronous Thrift transport/protocol stack.
"""

import logging
import os
import random
import sys
import threading
from pathlib import Path
from typing import Any

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
from mock_services.MockContentFilterService import (
    Client as MockContentFilterServiceClient,
)
from mock_services.MockRankingService import Client as MockRankingServiceClient
from mock_services.MockUserPreferenceService import (
    Client as MockUserPreferenceServiceClient,
)

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
        # Connect to HAProxy frontend on 9090 (load balances to 9100-9107)
        port = getattr(settings, "THRIFT_SERVER_PORT", 9090)
        return host, port
    except Exception:
        # Fallback to HAProxy frontend
        return "localhost", 9090


class ThriftConnectionPool:
    """
    Thread-safe connection pool for Thrift clients.

    Maintains persistent connections and reuses them instead of
    creating new sockets for every RPC call.
    """

    def __init__(self, host: str, port: int, pool_size: int = 10):
        """
        Initialize connection pool.

        Args:
            host: Thrift server host
            port: Thrift server port
            pool_size: Maximum number of connections to maintain
        """
        self.host = host
        self.port = port
        self.pool_size = pool_size
        self._pool = []
        self._lock = threading.Lock()
        self._local = threading.local()
        logger.debug(
            f"ThriftConnectionPool initialized - host={host}, port={port}, pool_size={pool_size}"
        )

    def _create_connection(self):
        """Create a new Thrift connection."""
        transport = TSocket.TSocket(self.host, self.port)
        transport = TTransport.TBufferedTransport(transport)
        protocol = TBinaryProtocol.TBinaryProtocol(transport)
        return transport, protocol

    def get_connection(self):
        """
        Get a connection from the pool or create a new one.

        Returns:
            (transport, protocol) tuple
        """
        with self._lock:
            if self._pool:
                transport, protocol = self._pool.pop()
                # Check if connection is still alive
                if transport.isOpen():
                    logger.debug("Reusing connection from pool")
                    return transport, protocol
                else:
                    logger.debug("Connection in pool is closed, creating new one")

            # Create new connection if pool is empty or connection is dead
            logger.debug("Creating new connection")
            transport, protocol = self._create_connection()
            try:
                transport.open()
            except Exception as e:
                logger.error(f"Failed to open connection: {e}")
                raise
            return transport, protocol

    def return_connection(self, transport, protocol):
        """
        Return a connection to the pool for reuse.

        Args:
            transport: Thrift transport to return
            protocol: Thrift protocol to return
        """
        with self._lock:
            if len(self._pool) < self.pool_size and transport.isOpen():
                logger.debug("Returning connection to pool")
                self._pool.append((transport, protocol))
            else:
                logger.debug("Pool full or connection closed, closing transport")
                try:
                    transport.close()
                except Exception:
                    pass  # Ignore errors during close

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


class ThriftAdsClient:
    """
    Thrift RPC client for MockAdsService with connection pooling.

    Reuses connections instead of creating new sockets for each RPC call.
    """

    # Global connection pool shared across all instances
    _connection_pool = None
    _pool_lock = threading.Lock()

    def __init__(self, host: str = "localhost", port: int = 9090):
        """Initialize Thrift client with connection pooling."""
        self.host = host
        self.port = port

        # Create global connection pool if it doesn't exist
        with ThriftAdsClient._pool_lock:
            if ThriftAdsClient._connection_pool is None:
                ThriftAdsClient._connection_pool = ThriftConnectionPool(
                    host=host, port=port, pool_size=20
                )

        logger.debug(f"ThriftAdsClient initialized - host={host}, port={port}")

    def fetch_ads(self, user_id: int, num_ads: int) -> ttypes.FetchAdsResponse:
        """
        Fetch ads via REAL Thrift RPC call using connection pooling.

        Reuses existing connections instead of creating new sockets!
        """
        transport = None
        protocol = None

        try:
            # Get connection from pool (reuses existing or creates new)
            transport, protocol = ThriftAdsClient._connection_pool.get_connection()

            # Create Thrift client with pooled connection
            client = MockAdsServiceClient(protocol)

            # Create Thrift request
            request = ttypes.FetchAdsRequest(
                user_id=user_id,
                num_ads_requested=num_ads,
                surface_type="FEED",
                context={},
            )

            # Make RPC call - triggers serialization + network + deserialization
            response = client.fetchAds(request)
            logger.debug(f"Fetched {len(response.ads)} ads for user {user_id}")

            # Return connection to pool for reuse
            ThriftAdsClient._connection_pool.return_connection(transport, protocol)

            return response

        except Exception as e:
            logger.error(f"Thrift RPC error in fetchAds: {e}")
            # Close broken connection instead of returning it to pool
            if transport:
                try:
                    transport.close()
                except Exception:
                    pass
            # Return empty response on error
            return ttypes.FetchAdsResponse(ads=[], total_fetched=0, request_id="error")


class ThriftRankingClient:
    """
    Thrift RPC client for MockRankingService with connection pooling.

    Reuses connections instead of creating new sockets for each RPC call.
    """

    # Global connection pool shared across all instances
    _connection_pool = None
    _pool_lock = threading.Lock()

    def __init__(self, host: str = "localhost", port: int = 9090):
        """Initialize Thrift client with connection pooling."""
        self.host = host
        self.port = port

        # Create global connection pool if it doesn't exist
        with ThriftRankingClient._pool_lock:
            if ThriftRankingClient._connection_pool is None:
                ThriftRankingClient._connection_pool = ThriftConnectionPool(
                    host=host, port=port, pool_size=20
                )

        logger.debug(f"ThriftRankingClient initialized - host={host}, port={port}")

    def rank_items(
        self, user_id: int, items: list, num_results: int
    ) -> ttypes.RankItemsResponse:
        """Rank items via REAL Thrift RPC call using connection pooling."""
        transport = None
        protocol = None

        try:
            # Get connection from pool
            transport, protocol = ThriftRankingClient._connection_pool.get_connection()

            # Create Thrift client with pooled connection
            client = MockRankingServiceClient(protocol)

            # Create request and make RPC call
            request = ttypes.RankItemsRequest(
                user_id=user_id,
                item_ids=[str(item) for item in items],
                num_results=num_results,
            )
            response = client.rankItems(request)
            logger.debug(f"Ranked {len(response.item_ids)} items for user {user_id}")

            # Return connection to pool for reuse
            ThriftRankingClient._connection_pool.return_connection(transport, protocol)

            return response

        except Exception as e:
            logger.error(f"Thrift RPC error in rankItems: {e}")
            # Close broken connection instead of returning it to pool
            if transport:
                try:
                    transport.close()
                except Exception:
                    pass
            return ttypes.RankItemsResponse(item_ids=[], scores=[], request_id="error")


class ThriftContentFilterClient:
    """
    Thrift RPC client for MockContentFilterService with connection pooling.

    Reuses connections instead of creating new sockets for each RPC call.
    """

    # Global connection pool shared across all instances
    _connection_pool = None
    _pool_lock = threading.Lock()

    def __init__(self, host: str = "localhost", port: int = 9090):
        """Initialize Thrift client with connection pooling."""
        self.host = host
        self.port = port

        # Create global connection pool if it doesn't exist
        with ThriftContentFilterClient._pool_lock:
            if ThriftContentFilterClient._connection_pool is None:
                ThriftContentFilterClient._connection_pool = ThriftConnectionPool(
                    host=host, port=port, pool_size=20
                )

        logger.debug(
            f"ThriftContentFilterClient initialized - host={host}, port={port}"
        )

    def filter_content(
        self, user_id: int, item_ids: list, filter_level: str = "moderate"
    ) -> ttypes.FilterContentResponse:
        """Filter content via REAL Thrift RPC call using connection pooling."""
        transport = None
        protocol = None

        try:
            # Get connection from pool
            transport, protocol = (
                ThriftContentFilterClient._connection_pool.get_connection()
            )

            # Create Thrift client with pooled connection
            client = MockContentFilterServiceClient(protocol)

            # Create request and make RPC call
            request = ttypes.FilterContentRequest(
                user_id=user_id,
                item_ids=[str(item) for item in item_ids],
                filter_level=filter_level,
            )
            response = client.filterContent(request)
            logger.debug(f"Filtered {response.total_filtered} items for user {user_id}")

            # Return connection to pool for reuse
            ThriftContentFilterClient._connection_pool.return_connection(
                transport, protocol
            )

            return response

        except Exception as e:
            logger.error(f"Thrift RPC error in filterContent: {e}")
            # Close broken connection instead of returning it to pool
            if transport:
                try:
                    transport.close()
                except Exception:
                    pass
            return ttypes.FilterContentResponse(
                safe_item_ids=[],
                blocked_item_ids=[],
                total_filtered=0,
                request_id="error",
            )


class ThriftUserPreferenceClient:
    """
    Thrift RPC client for MockUserPreferenceService with connection pooling.

    Reuses connections instead of creating new sockets for each RPC call.
    """

    # Global connection pool shared across all instances
    _connection_pool = None
    _pool_lock = threading.Lock()

    def __init__(self, host: str = "localhost", port: int = 9090):
        """Initialize Thrift client with connection pooling."""
        self.host = host
        self.port = port

        # Create global connection pool if it doesn't exist
        with ThriftUserPreferenceClient._pool_lock:
            if ThriftUserPreferenceClient._connection_pool is None:
                ThriftUserPreferenceClient._connection_pool = ThriftConnectionPool(
                    host=host, port=port, pool_size=20
                )

        logger.debug(
            f"ThriftUserPreferenceClient initialized - host={host}, port={port}"
        )

    def get_user_preferences(self, user_id: int) -> ttypes.UserPreferencesResponse:
        """Get user preferences via REAL Thrift RPC call using connection pooling."""
        transport = None
        protocol = None

        try:
            # Get connection from pool
            transport, protocol = (
                ThriftUserPreferenceClient._connection_pool.get_connection()
            )

            # Create Thrift client with pooled connection
            client = MockUserPreferenceServiceClient(protocol)

            # Create request and make RPC call
            request = ttypes.UserPreferencesRequest(user_id=user_id)
            response = client.getUserPreferences(request)
            logger.debug(f"Fetched preferences for user {user_id}")

            # Return connection to pool for reuse
            ThriftUserPreferenceClient._connection_pool.return_connection(
                transport, protocol
            )

            return response

        except Exception as e:
            logger.error(f"Thrift RPC error in getUserPreferences: {e}")
            # Close broken connection instead of returning it to pool
            if transport:
                try:
                    transport.close()
                except Exception:
                    pass
            return ttypes.UserPreferencesResponse(
                preferences={}, favorite_topics=[], request_id="error"
            )


class AdData:
    """
    Simple wrapper for ad data extracted from Thrift response.
    Used to pass ad data to Django workload steps.
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

    def to_dict(self):
        """Convert to dictionary format for Django workload."""
        return {
            "pk": f"ad_{self.ad_id}",
            "comment_count": self.comment_count,
            "published": 0.0,
            "user": {
                "name": f"Advertiser_{self.advertiser_id}",
                "pk": f"advertiser_{self.advertiser_id}",
            },
            "is_ad": True,
            "ad_title": self.ad_title,
            "ad_score": self.quality_score,
            "predicted_ctr": self.predicted_ctr,
            "predicted_cvr": self.predicted_cvr,
        }


# Global client instances (reusable connection parameters)
_ads_client_instance = None
_ranking_client_instance = None
_filter_client_instance = None
_pref_client_instance = None


def get_ads_client() -> ThriftAdsClient:
    """Get or create global Thrift ads client instance."""
    global _ads_client_instance
    if _ads_client_instance is None:
        host, port = _get_thrift_server_config()
        _ads_client_instance = ThriftAdsClient(host=host, port=port)
    return _ads_client_instance


def get_ranking_client() -> ThriftRankingClient:
    """Get or create global Thrift ranking client instance."""
    global _ranking_client_instance
    if _ranking_client_instance is None:
        host, port = _get_thrift_server_config()
        _ranking_client_instance = ThriftRankingClient(host=host, port=port)
    return _ranking_client_instance


def get_filter_client() -> ThriftContentFilterClient:
    """Get or create global Thrift content filter client instance."""
    global _filter_client_instance
    if _filter_client_instance is None:
        host, port = _get_thrift_server_config()
        _filter_client_instance = ThriftContentFilterClient(host=host, port=port)
    return _filter_client_instance


def get_preference_client() -> ThriftUserPreferenceClient:
    """Get or create global Thrift user preference client instance."""
    global _pref_client_instance
    if _pref_client_instance is None:
        host, port = _get_thrift_server_config()
        _pref_client_instance = ThriftUserPreferenceClient(host=host, port=port)
    return _pref_client_instance

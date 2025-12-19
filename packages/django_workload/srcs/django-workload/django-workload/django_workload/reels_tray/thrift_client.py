# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
Thrift RPC Client for Reels Tray Services.

This module provides Thrift clients for:
- Tray ranking via ML ranking pipelines
- User metadata fetching via data access framework

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


class TrayRankingData:
    """
    Wrapper for tray ranking data extracted from Thrift response.
    Used to pass ranked tray positions to the service.
    """

    def __init__(self, user_id: str, rank: int, score: float, is_live: bool = False):
        """Initialize tray ranking data."""
        self.user_id = user_id
        self.rank = rank
        self.score = score
        self.is_live = is_live

    def to_dict(self) -> Dict[str, Any]:
        """Convert to dictionary format for service use."""
        return {
            "user_id": self.user_id,
            "rank": self.rank,
            "score": self.score,
            "is_live": self.is_live,
        }


class UserMetadata:
    """
    Wrapper for user metadata extracted from Thrift response.
    Models the lazy user dictionary pattern in production.
    """

    def __init__(
        self,
        user_id: str,
        username: str,
        full_name: str,
        profile_pic_url: str,
        is_verified: bool = False,
        is_private: bool = False,
    ):
        """Initialize user metadata."""
        self.user_id = user_id
        self.username = username
        self.full_name = full_name
        self.profile_pic_url = profile_pic_url
        self.is_verified = is_verified
        self.is_private = is_private

    def to_dict(self) -> Dict[str, Any]:
        """Convert to dictionary format for JSON serialization."""
        return {
            "pk": self.user_id,
            "username": self.username,
            "full_name": self.full_name,
            "profile_pic_url": self.profile_pic_url,
            "is_verified": self.is_verified,
            "is_private": self.is_private,
        }


class ThriftTrayRankingClient:
    """
    Thrift RPC client for Tray Ranking Service with connection pooling.

    Ranks users for the stories/reels tray using ML ranking pipelines.
    """

    _connection_pool = None
    _pool_lock = threading.Lock()

    def __init__(self, host: str = "localhost", port: int = 9090):
        """Initialize Thrift client with connection pooling."""
        self.host = host
        self.port = port

        with ThriftTrayRankingClient._pool_lock:
            if ThriftTrayRankingClient._connection_pool is None:
                ThriftTrayRankingClient._connection_pool = ThriftConnectionPool(
                    host=host, port=port, pool_size=20
                )

        logger.debug(f"ThriftTrayRankingClient initialized - host={host}, port={port}")

    def rank_tray_users(
        self,
        viewer_id: int,
        user_ids: List[str],
        num_results: int,
        include_live: bool = True,
    ) -> List[TrayRankingData]:
        """
        Rank users for tray display via REAL Thrift RPC call.

        Args:
            viewer_id: Viewer's user ID for personalized ranking
            user_ids: List of user IDs to rank
            num_results: Number of results to return
            include_live: Whether to include live stories

        Returns:
            List of TrayRankingData objects sorted by rank
        """
        transport = None
        protocol = None

        try:
            transport, protocol = (
                ThriftTrayRankingClient._connection_pool.get_connection()
            )
            client = MockRankingServiceClient(protocol)

            request = ttypes.RankItemsRequest(
                user_id=viewer_id,
                item_ids=user_ids,
                num_results=num_results,
            )

            response = client.rankItems(request)
            logger.debug(f"Ranked {len(response.item_ids)} tray users for {viewer_id}")

            ThriftTrayRankingClient._connection_pool.return_connection(
                transport, protocol
            )

            # Convert to TrayRankingData
            results = []
            for i, (user_id, score) in enumerate(
                zip(response.item_ids, response.scores)
            ):
                results.append(
                    TrayRankingData(
                        user_id=user_id,
                        rank=i,
                        score=score,
                        is_live=(i < 3 and include_live),  # First few might be live
                    )
                )

            return results

        except Exception as e:
            logger.error(f"Thrift RPC error in rank_tray_users: {e}")
            if transport:
                try:
                    transport.close()
                except Exception:
                    pass
            return []


class ThriftUserMetadataClient:
    """
    Thrift RPC client for User Metadata Service with connection pooling.

    Fetches user metadata for tray display using data access framework patterns.
    """

    _connection_pool = None
    _pool_lock = threading.Lock()

    def __init__(self, host: str = "localhost", port: int = 9090):
        """Initialize Thrift client with connection pooling."""
        self.host = host
        self.port = port

        with ThriftUserMetadataClient._pool_lock:
            if ThriftUserMetadataClient._connection_pool is None:
                ThriftUserMetadataClient._connection_pool = ThriftConnectionPool(
                    host=host, port=port, pool_size=20
                )

        logger.debug(f"ThriftUserMetadataClient initialized - host={host}, port={port}")

    def get_user_metadata_batch(self, user_ids: List[str]) -> Dict[str, UserMetadata]:
        """
        Fetch user metadata in batch via Thrift RPC call.

        Args:
            user_ids: List of user IDs to fetch metadata for

        Returns:
            Dict mapping user_id to UserMetadata
        """
        transport = None
        protocol = None

        try:
            transport, protocol = (
                ThriftUserMetadataClient._connection_pool.get_connection()
            )
            client = MockUserPreferenceServiceClient(protocol)

            # Use the existing UserPreferences RPC as a proxy for user metadata
            # In production, this would be the data access framework
            results = {}
            for user_id in user_ids:
                try:
                    request = ttypes.UserPreferencesRequest(user_id=int(user_id))
                    response = client.getUserPreferences(request)

                    # Build UserMetadata from response
                    results[user_id] = UserMetadata(
                        user_id=user_id,
                        username=f"user_{user_id}",
                        full_name=response.preferences.get(
                            "display_name", f"User {user_id}"
                        ),
                        profile_pic_url=f"https://cdn.example.com/profiles/{user_id}.jpg",
                        is_verified=(int(user_id) % 10 == 0),  # 10% verified
                        is_private=(int(user_id) % 5 == 0),  # 20% private
                    )
                except Exception:
                    # Generate fallback metadata
                    results[user_id] = UserMetadata(
                        user_id=user_id,
                        username=f"user_{user_id}",
                        full_name=f"User {user_id}",
                        profile_pic_url=f"https://cdn.example.com/profiles/default.jpg",
                        is_verified=False,
                        is_private=False,
                    )

            logger.debug(f"Fetched metadata for {len(results)} users")

            ThriftUserMetadataClient._connection_pool.return_connection(
                transport, protocol
            )

            return results

        except Exception as e:
            logger.error(f"Thrift RPC error in get_user_metadata_batch: {e}")
            if transport:
                try:
                    transport.close()
                except Exception:
                    pass
            return {}


# Global client instances
_tray_ranking_client_instance = None
_user_metadata_client_instance = None


def get_tray_ranking_client() -> ThriftTrayRankingClient:
    """Get or create global Thrift tray ranking client instance."""
    global _tray_ranking_client_instance
    if _tray_ranking_client_instance is None:
        host, port = _get_thrift_server_config()
        _tray_ranking_client_instance = ThriftTrayRankingClient(host=host, port=port)
    return _tray_ranking_client_instance


def get_user_metadata_client() -> ThriftUserMetadataClient:
    """Get or create global Thrift user metadata client instance."""
    global _user_metadata_client_instance
    if _user_metadata_client_instance is None:
        host, port = _get_thrift_server_config()
        _user_metadata_client_instance = ThriftUserMetadataClient(host=host, port=port)
    return _user_metadata_client_instance

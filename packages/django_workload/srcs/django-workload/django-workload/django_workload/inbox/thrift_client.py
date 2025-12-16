# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
Thrift RPC Client for Inbox Services.

This module provides Thrift clients for:
- Thread fetching and message previews
- Spam filtering service
- User metadata fetching via NodeAPI/LazyUserDict
- PubSub subscription for real-time updates

Uses connection pooling to reuse connections instead of creating new
sockets for every RPC call.
"""

import logging
import os
import sys
import threading
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

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
from mock_services.MockInboxService import Client as MockInboxServiceClient

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


# ============================================================================
# Connection Pool
# ============================================================================


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


# ============================================================================
# Data Classes for RPC Responses
# ============================================================================


@dataclass
class InboxThreadData:
    """Thread data returned from inbox service."""

    thread_id: str
    participant_ids: List[str]
    last_activity_at: int
    unread_count: int
    is_spam: bool = False
    is_muted: bool = False
    thread_type: str = "private"
    title: Optional[str] = None

    def to_dict(self) -> Dict[str, Any]:
        return {
            "thread_id": self.thread_id,
            "participant_ids": self.participant_ids,
            "last_activity_at": self.last_activity_at,
            "unread_count": self.unread_count,
            "is_spam": self.is_spam,
            "is_muted": self.is_muted,
            "thread_type": self.thread_type,
            "title": self.title,
        }


@dataclass
class InboxMessagePreview:
    """Message preview returned from inbox service."""

    message_id: str
    thread_id: str
    sender_id: str
    text_preview: str
    timestamp: int
    message_type: str = "text"
    is_unsent: bool = False

    def to_dict(self) -> Dict[str, Any]:
        return {
            "message_id": self.message_id,
            "thread_id": self.thread_id,
            "sender_id": self.sender_id,
            "text_preview": self.text_preview,
            "timestamp": self.timestamp,
            "message_type": self.message_type,
            "is_unsent": self.is_unsent,
        }


@dataclass
class InboxUserMetadata:
    """User metadata for inbox participants."""

    user_id: str
    username: str
    full_name: str
    profile_pic_url: str
    is_verified: bool = False
    is_private: bool = False
    presence_status: str = "offline"
    last_active_at: Optional[int] = None

    def to_dict(self) -> Dict[str, Any]:
        return {
            "pk": self.user_id,
            "username": self.username,
            "full_name": self.full_name,
            "profile_pic_url": self.profile_pic_url,
            "is_verified": self.is_verified,
            "is_private": self.is_private,
            "presence_status": self.presence_status,
            "last_active_at": self.last_active_at,
        }


@dataclass
class SpamCheckResult:
    """Result from spam filtering service."""

    thread_id: str
    is_spam: bool
    spam_score: float
    spam_reason: Optional[str] = None


@dataclass
class PubSubSubscriptionState:
    """State from PubSub real-time subscription."""

    sequence_id: int
    snapshot_at: int
    has_pending_updates: bool
    pending_thread_ids: List[str] = field(default_factory=list)


# ============================================================================
# Thrift Client Classes
# ============================================================================


class InboxThreadServiceClient:
    """
    Client for fetching inbox threads via Thrift RPC.

    Models calls to the Direct inbox service that fetches
    thread lists and message previews.
    """

    _connection_pool = None
    _pool_lock = threading.Lock()

    def __init__(self, host: str = "localhost", port: int = 9090):
        self.host = host
        self.port = port

        with InboxThreadServiceClient._pool_lock:
            if InboxThreadServiceClient._connection_pool is None:
                InboxThreadServiceClient._connection_pool = ThriftConnectionPool(
                    host=host, port=port, pool_size=20
                )

        logger.debug(f"InboxThreadServiceClient initialized - host={host}, port={port}")

    def get_threads(
        self,
        viewer_id: int,
        cursor: Optional[str] = None,
        page_size: int = 20,
        include_spam: bool = False,
    ) -> List[InboxThreadData]:
        """
        Fetch inbox threads for a user via REAL Thrift RPC call.

        Args:
            viewer_id: The viewing user's ID
            cursor: Pagination cursor
            page_size: Number of threads to fetch
            include_spam: Whether to include spam threads

        Returns:
            List of thread data
        """
        transport = None
        protocol = None

        try:
            transport, protocol = (
                InboxThreadServiceClient._connection_pool.get_connection()
            )
            client = MockInboxServiceClient(protocol)

            request = ttypes.InboxGetThreadsRequest(
                viewer_id=viewer_id,
                cursor=cursor,
                page_size=page_size,
                include_spam=include_spam,
            )

            response = client.getThreads(request)
            logger.debug(
                f"Fetched {len(response.threads)} threads for viewer {viewer_id}"
            )

            InboxThreadServiceClient._connection_pool.return_connection(
                transport, protocol
            )

            # Convert Thrift response to InboxThreadData
            threads = []
            for t in response.threads:
                threads.append(
                    InboxThreadData(
                        thread_id=t.thread_id,
                        participant_ids=list(t.participant_ids),
                        last_activity_at=t.last_activity_at,
                        unread_count=t.unread_count,
                        is_spam=t.is_spam,
                        is_muted=t.is_muted,
                        thread_type=t.thread_type,
                        title=t.title,
                    )
                )

            return threads

        except Exception as e:
            logger.error(f"Thrift RPC error in get_threads: {e}")
            if transport:
                try:
                    transport.close()
                except Exception:
                    pass
            return []

    def get_message_previews(
        self,
        thread_ids: List[str],
        messages_per_thread: int = 1,
    ) -> Dict[str, List[InboxMessagePreview]]:
        """
        Fetch message previews for threads via Thrift RPC.

        Args:
            thread_ids: List of thread IDs
            messages_per_thread: Number of messages per thread

        Returns:
            Dict mapping thread_id to list of message previews
        """
        transport = None
        protocol = None

        try:
            transport, protocol = (
                InboxThreadServiceClient._connection_pool.get_connection()
            )
            client = MockInboxServiceClient(protocol)

            request = ttypes.InboxMessagePreviewsRequest(
                thread_ids=thread_ids,
                messages_per_thread=messages_per_thread,
            )

            response = client.getMessagePreviews(request)
            logger.debug(f"Fetched message previews for {len(thread_ids)} threads")

            InboxThreadServiceClient._connection_pool.return_connection(
                transport, protocol
            )

            # Convert Thrift response to InboxMessagePreview
            previews = {}
            for thread_id, messages in response.previews.items():
                previews[thread_id] = [
                    InboxMessagePreview(
                        message_id=m.message_id,
                        thread_id=m.thread_id,
                        sender_id=m.sender_id,
                        text_preview=m.text_preview,
                        timestamp=m.timestamp,
                        message_type=m.message_type,
                        is_unsent=m.is_unsent,
                    )
                    for m in messages
                ]

            return previews

        except Exception as e:
            logger.error(f"Thrift RPC error in get_message_previews: {e}")
            if transport:
                try:
                    transport.close()
                except Exception:
                    pass
            return {}


class SpamFilteringClient:
    """
    Client for spam filtering service via Thrift RPC.

    Models calls to spam detection service for inbox threads.
    """

    _connection_pool = None
    _pool_lock = threading.Lock()

    def __init__(self, host: str = "localhost", port: int = 9090):
        self.host = host
        self.port = port

        with SpamFilteringClient._pool_lock:
            if SpamFilteringClient._connection_pool is None:
                SpamFilteringClient._connection_pool = ThriftConnectionPool(
                    host=host, port=port, pool_size=20
                )

        logger.debug(f"SpamFilteringClient initialized - host={host}, port={port}")

    def check_threads_batch(
        self,
        thread_ids: List[str],
        viewer_id: int,
    ) -> Dict[str, SpamCheckResult]:
        """
        Check multiple threads for spam via Thrift RPC.

        Args:
            thread_ids: List of thread IDs to check
            viewer_id: The viewing user's ID

        Returns:
            Dict mapping thread_id to spam check result
        """
        transport = None
        protocol = None

        try:
            transport, protocol = SpamFilteringClient._connection_pool.get_connection()
            client = MockInboxServiceClient(protocol)

            request = ttypes.InboxSpamCheckRequest(
                thread_ids=thread_ids,
                viewer_id=viewer_id,
            )

            response = client.checkThreadsSpam(request)
            logger.debug(f"Checked spam for {len(thread_ids)} threads")

            SpamFilteringClient._connection_pool.return_connection(transport, protocol)

            # Convert Thrift response to SpamCheckResult
            results = {}
            for thread_id, result in response.results.items():
                results[thread_id] = SpamCheckResult(
                    thread_id=result.thread_id,
                    is_spam=result.is_spam,
                    spam_score=result.spam_score,
                    spam_reason=result.spam_reason,
                )

            return results

        except Exception as e:
            logger.error(f"Thrift RPC error in check_threads_batch: {e}")
            if transport:
                try:
                    transport.close()
                except Exception:
                    pass
            return {}


class PubSubSubscriptionClient:
    """
    Client for PubSub real-time subscription service.

    Models calls to PubSub for real-time inbox updates
    and resnapshot triggers.
    """

    _connection_pool = None
    _pool_lock = threading.Lock()

    def __init__(self, host: str = "localhost", port: int = 9090):
        self.host = host
        self.port = port

        with PubSubSubscriptionClient._pool_lock:
            if PubSubSubscriptionClient._connection_pool is None:
                PubSubSubscriptionClient._connection_pool = ThriftConnectionPool(
                    host=host, port=port, pool_size=20
                )

        logger.debug(f"PubSubSubscriptionClient initialized - host={host}, port={port}")

    def get_subscription_state(
        self,
        viewer_id: int,
    ) -> PubSubSubscriptionState:
        """
        Get current PubSub subscription state via Thrift RPC.

        Args:
            viewer_id: The viewing user's ID

        Returns:
            Current subscription state
        """
        transport = None
        protocol = None

        try:
            transport, protocol = (
                PubSubSubscriptionClient._connection_pool.get_connection()
            )
            client = MockInboxServiceClient(protocol)

            request = ttypes.IrisSubscriptionStateRequest(
                viewer_id=viewer_id,
            )

            response = client.getIrisState(request)
            logger.debug(f"Fetched PubSub state for viewer {viewer_id}")

            PubSubSubscriptionClient._connection_pool.return_connection(
                transport, protocol
            )

            # Convert Thrift response to IrisSubscriptionState
            state = response.state
            return PubSubSubscriptionState(
                sequence_id=state.sequence_id,
                snapshot_at=state.snapshot_at,
                has_pending_updates=state.has_pending_updates,
                pending_thread_ids=list(state.pending_thread_ids),
            )

        except Exception as e:
            logger.error(f"Thrift RPC error in get_subscription_state: {e}")
            if transport:
                try:
                    transport.close()
                except Exception:
                    pass
            return PubSubSubscriptionState(
                sequence_id=0,
                snapshot_at=0,
                has_pending_updates=False,
            )


class InboxUserMetadataClient:
    """
    Client for user metadata service via Thrift RPC.

    Models calls to fetch user metadata for inbox participants.
    """

    _connection_pool = None
    _pool_lock = threading.Lock()

    def __init__(self, host: str = "localhost", port: int = 9090):
        self.host = host
        self.port = port

        with InboxUserMetadataClient._pool_lock:
            if InboxUserMetadataClient._connection_pool is None:
                InboxUserMetadataClient._connection_pool = ThriftConnectionPool(
                    host=host, port=port, pool_size=20
                )

        logger.debug(f"InboxUserMetadataClient initialized - host={host}, port={port}")

    def get_user_metadata_batch(
        self,
        user_ids: List[str],
    ) -> Dict[str, InboxUserMetadata]:
        """
        Fetch metadata for multiple users via Thrift RPC.

        Args:
            user_ids: List of user IDs

        Returns:
            Dict mapping user_id to metadata
        """
        transport = None
        protocol = None

        try:
            transport, protocol = (
                InboxUserMetadataClient._connection_pool.get_connection()
            )
            client = MockInboxServiceClient(protocol)

            request = ttypes.InboxUserMetadataRequest(
                user_ids=user_ids,
                viewer_id=0,  # Not used in mock
            )

            response = client.getUserMetadata(request)
            logger.debug(f"Fetched metadata for {len(user_ids)} users")

            InboxUserMetadataClient._connection_pool.return_connection(
                transport, protocol
            )

            # Convert Thrift response to InboxUserMetadata
            metadata = {}
            for user_id, meta in response.metadata.items():
                metadata[user_id] = InboxUserMetadata(
                    user_id=meta.user_id,
                    username=meta.username,
                    full_name=meta.full_name,
                    profile_pic_url=meta.profile_pic_url,
                    is_verified=meta.is_verified,
                    is_private=meta.is_private,
                    presence_status=meta.presence_status,
                    last_active_at=meta.last_active_at,
                )

            return metadata

        except Exception as e:
            logger.error(f"Thrift RPC error in get_user_metadata_batch: {e}")
            if transport:
                try:
                    transport.close()
                except Exception:
                    pass
            return {}


# ============================================================================
# Client Factory Functions
# ============================================================================

# Global client instances
_inbox_thread_client_instance = None
_spam_filtering_client_instance = None
_pubsub_subscription_client_instance = None
_inbox_user_metadata_client_instance = None


def get_inbox_thread_client() -> InboxThreadServiceClient:
    """Get or create global inbox thread service client instance."""
    global _inbox_thread_client_instance
    if _inbox_thread_client_instance is None:
        host, port = _get_thrift_server_config()
        _inbox_thread_client_instance = InboxThreadServiceClient(host=host, port=port)
    return _inbox_thread_client_instance


def get_spam_filtering_client() -> SpamFilteringClient:
    """Get or create global spam filtering service client instance."""
    global _spam_filtering_client_instance
    if _spam_filtering_client_instance is None:
        host, port = _get_thrift_server_config()
        _spam_filtering_client_instance = SpamFilteringClient(host=host, port=port)
    return _spam_filtering_client_instance


def get_pubsub_subscription_client() -> PubSubSubscriptionClient:
    """Get or create global PubSub subscription service client instance."""
    global _pubsub_subscription_client_instance
    if _pubsub_subscription_client_instance is None:
        host, port = _get_thrift_server_config()
        _pubsub_subscription_client_instance = PubSubSubscriptionClient(
            host=host, port=port
        )
    return _pubsub_subscription_client_instance


def get_inbox_user_metadata_client() -> InboxUserMetadataClient:
    """Get or create global user metadata service client instance."""
    global _inbox_user_metadata_client_instance
    if _inbox_user_metadata_client_instance is None:
        host, port = _get_thrift_server_config()
        _inbox_user_metadata_client_instance = InboxUserMetadataClient(
            host=host, port=port
        )
    return _inbox_user_metadata_client_instance

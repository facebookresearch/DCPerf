# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

"""
Inbox module for DjangoBench V2.

Provides InboxService for modeling the workload of activity.api.views.inbox
from production IG Django server. This module handles:
- Thread and message aggregation from Direct cache and database
- User metadata fetching via NodeAPI/LazyUserDict patterns
- Spam filtering via microservice calls
- Real-time updates via PubSub subscriptions
- Read state management and badge calculations
- Cursor-based pagination
"""

# Primitives - based on production leaf function profiles
from .primitives import (
    ConfigConstructionPrimitives,
    execute_inbox_random_primitives,
    ExperimentationPrimitives,
    ExperimentResolverPrimitives,
    FeatureFlagPrimitives,
    FeatureGatingPrimitives,
    get_inbox_primitive_methods,
    INBOX_PRIMITIVE_WEIGHTS,
    InboxPrimitives,
    MemoizationPrimitives,
    MetricsCollectionPrimitives,
    NotificationRenderPrimitives,
    PropertyAccessPrimitives,
    SchemaValidationPrimitives,
    TypeCachingPrimitives,
    ViewerContextPrimitives,
)

# Base service classes
from .service import (
    InboxContext,
    InboxRequest,
    InboxResponse,
    InboxService,
    InboxThread,
)

# Thrift clients
from .thrift_client import (
    get_inbox_thread_client,
    get_inbox_user_metadata_client,
    get_pubsub_subscription_client,
    get_spam_filtering_client,
    InboxMessagePreview,
    InboxThreadData,
    InboxUserMetadata,
    PubSubSubscriptionState,
    SpamCheckResult,
)

# All exports
__all__ = [
    # Primitives - based on production leaf function profiles
    "ConfigConstructionPrimitives",
    "execute_inbox_random_primitives",
    "ExperimentationPrimitives",
    "ExperimentResolverPrimitives",
    "FeatureFlagPrimitives",
    "FeatureGatingPrimitives",
    "get_inbox_primitive_methods",
    "INBOX_PRIMITIVE_WEIGHTS",
    "InboxPrimitives",
    "MemoizationPrimitives",
    "MetricsCollectionPrimitives",
    "NotificationRenderPrimitives",
    "PropertyAccessPrimitives",
    "SchemaValidationPrimitives",
    "TypeCachingPrimitives",
    "ViewerContextPrimitives",
    # Service classes
    "InboxContext",
    "InboxRequest",
    "InboxResponse",
    "InboxService",
    "InboxThread",
    # Thrift clients
    "get_inbox_thread_client",
    "get_inbox_user_metadata_client",
    "get_pubsub_subscription_client",
    "get_spam_filtering_client",
    "InboxMessagePreview",
    "InboxThreadData",
    "InboxUserMetadata",
    "PubSubSubscriptionState",
    "SpamCheckResult",
]

# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

# models represent mock data, here to drive Python and Cassandra to produce
# reasonably realistic I/O.
import datetime
import enum
import uuid

from cassandra.cqlengine import columns
from cassandra.util import datetime_from_uuid1, uuid_from_time
from django_cassandra_engine.models import DjangoCassandraModel


def timeuuid_now():
    return uuid_from_time(datetime.datetime.now())


class UserModel(DjangoCassandraModel):
    id = columns.UUID(primary_key=True, default=uuid.uuid4)
    name = columns.Text()
    following = columns.List(columns.UUID)

    def feed_entries(self):
        return FeedEntryModel.objects(userid=self.id)

    @property
    def json_data(self):
        return {"name": self.name, "pk": str(self.id)}

    # allow this to be used as request.user without breaking expectations
    def is_authenticated(self):
        return True


class FeedEntryModel(DjangoCassandraModel):
    class Meta:
        get_pk_field = "id"

    userid = columns.UUID(primary_key=True)
    id = columns.TimeUUID(
        primary_key=True, default=timeuuid_now, clustering_order="DESC"
    )
    comment_count = columns.SmallInt(default=0)

    @property
    def published(self):
        return datetime_from_uuid1(self.id)


class BundleEntryModel(DjangoCassandraModel):
    class Meta:
        get_pk_field = "id"

    userid = columns.UUID(primary_key=True)
    id = columns.TimeUUID(
        primary_key=True, default=timeuuid_now, clustering_order="DESC"
    )
    comment_count = columns.SmallInt(default=0)
    entry_ids = columns.List(columns.UUID)

    @property
    def published(self):
        return datetime_from_uuid1(self.id)


class BundleSeenModel(DjangoCassandraModel):
    class Meta:
        # required but meaningless in this context
        get_pk_field = "userid"

    userid = columns.UUID(primary_key=True)
    bundleid = columns.UUID(primary_key=True)
    ts = columns.TimeUUID(
        primary_key=True, default=timeuuid_now, clustering_order="DESC"
    )
    entryid = columns.UUID()


class FeedSeenModel(DjangoCassandraModel):
    """
    Tracks which feed entries a user has seen.
    Used for deduplication and seen state tracking in feed timeline.
    """

    class Meta:
        get_pk_field = "userid"

    userid = columns.UUID(primary_key=True)
    entryid = columns.UUID(primary_key=True)
    seen_at = columns.TimeUUID(default=timeuuid_now)

    @property
    def json_data(self):
        return {
            "userid": str(self.userid),
            "entryid": str(self.entryid),
            "seen_at": str(datetime_from_uuid1(self.seen_at)),
        }


class InboxTypes(enum.Enum):
    COMMENT = "comment"
    FOLLOWER = "follower"
    LIKE = "like"


class InboxEntryBase(DjangoCassandraModel):
    __table_name__ = "inbox_entries"

    class Meta:
        get_pk_field = "id"

    userid = columns.UUID(primary_key=True)
    id = columns.TimeUUID(
        primary_key=True, default=timeuuid_now, clustering_order="DESC"
    )
    inbox_type = columns.Text(discriminator_column=True)

    @property
    def published(self):
        return datetime_from_uuid1(self.id)

    json_fields = {}

    @property
    def json_data(self):
        data = {
            "pk": str(self.id),
            "type": self.type.value,
            "published": str(self.published),
        }
        for key, colname in self.json_fields.items():
            data[key] = getattr(self, colname)
        return data


class CommentedInboxEntryModel(InboxEntryBase):
    type = InboxTypes.COMMENT
    __discriminator_value__ = type.value

    feedentryid = columns.TimeUUID()
    comment_text = columns.Text()
    json_fields = {"text": "comment_text"}


class LikeInboxEntryModel(InboxEntryBase):
    type = InboxTypes.LIKE
    __discriminator_value__ = type.value

    feedentryid = columns.TimeUUID()
    likerid = columns.UUID()
    json_fields = {"feedentryid": "feedentryid", "likerid": "likerid"}


class NewFollowerInboxEntryModel(InboxEntryBase):
    type = InboxTypes.FOLLOWER
    __discriminator_value__ = type.value

    followerid = columns.UUID()
    json_fields = {"followerid": "followerid"}


# ============================================================================
# Clips/Reels Models for clips.api.views.async_stream_clips_discover
# ============================================================================


class ClipVideoModel(DjangoCassandraModel):
    """
    Represents a video entry (Reel/Clip) in the system.
    Models the video metadata stored in IG's clips/reels inventory.
    Each video has multiple chunks for progressive loading.
    """

    class Meta:
        get_pk_field = "id"

    id = columns.UUID(primary_key=True, default=uuid.uuid4)
    owner_id = columns.UUID()
    title = columns.Text()
    description = columns.Text()
    duration_ms = columns.Integer()
    view_count = columns.BigInt(default=0)
    like_count = columns.BigInt(default=0)
    comment_count = columns.Integer(default=0)
    share_count = columns.Integer(default=0)
    created_at = columns.TimeUUID(default=timeuuid_now)
    thumbnail_url = columns.Text()
    is_published = columns.Boolean(default=True)
    content_type = columns.Text(default="reel")
    audio_track_id = columns.UUID()
    hashtags = columns.List(columns.Text)
    quality_score = columns.Float(default=0.5)
    engagement_score = columns.Float(default=0.5)

    @property
    def published(self):
        return datetime_from_uuid1(self.created_at)

    @property
    def json_data(self):
        return {
            "pk": str(self.id),
            "owner_id": str(self.owner_id),
            "title": self.title,
            "description": self.description,
            "duration_ms": self.duration_ms,
            "view_count": self.view_count,
            "like_count": self.like_count,
            "comment_count": self.comment_count,
            "share_count": self.share_count,
            "thumbnail_url": self.thumbnail_url,
            "content_type": self.content_type,
            "quality_score": self.quality_score,
            "engagement_score": self.engagement_score,
            "published": str(self.published),
        }


class ClipChunkModel(DjangoCassandraModel):
    """
    Represents a video chunk for progressive streaming.
    Models how video content is segmented for delivery.
    Each chunk contains a portion of the video data.
    """

    class Meta:
        get_pk_field = "chunk_id"

    chunk_id = columns.UUID(primary_key=True, default=uuid.uuid4)
    video_id = columns.UUID(index=True)
    chunk_index = columns.Integer()
    chunk_url = columns.Text()
    chunk_size_bytes = columns.Integer()
    duration_ms = columns.Integer()
    start_time_ms = columns.Integer()
    end_time_ms = columns.Integer()
    resolution = columns.Text(default="1080p")
    bitrate_kbps = columns.Integer()
    codec = columns.Text(default="h264")

    @property
    def json_data(self):
        return {
            "chunk_id": str(self.chunk_id),
            "video_id": str(self.video_id),
            "chunk_index": self.chunk_index,
            "chunk_url": self.chunk_url,
            "chunk_size_bytes": self.chunk_size_bytes,
            "duration_ms": self.duration_ms,
            "start_time_ms": self.start_time_ms,
            "end_time_ms": self.end_time_ms,
            "resolution": self.resolution,
            "bitrate_kbps": self.bitrate_kbps,
        }


class ClipSeenModel(DjangoCassandraModel):
    """
    Tracks which clips a user has seen.
    Used for deduplication and pagination in clips discovery.
    """

    class Meta:
        get_pk_field = "userid"

    userid = columns.UUID(primary_key=True)
    video_id = columns.UUID(primary_key=True)
    seen_at = columns.TimeUUID(default=timeuuid_now)
    watch_duration_ms = columns.Integer(default=0)
    completed = columns.Boolean(default=False)


# ============================================================================
# Bundle Reel Clips Model for feed.api.views.reels_tray
# ============================================================================


class BundleReelClipModel(DjangoCassandraModel):
    """
    Associates bundle entries (stories/reels tray items) with video clips.
    Models the relationship between tray buckets and their media content.

    In production reels_tray:
    - Each bucket in the tray represents a user with active stories/reels
    - Buckets can contain multiple video clips (stories/reels)
    - Only the first N buckets are fully materialized with media data
    - Remaining buckets are skeletons (minimal info, no media)

    This model supports the partial materialization pattern where
    _fetch_reel_clips_for_bundles() only fetches clips for the first
    NUM_FILLED_BUCKETS (typically 4) buckets.
    """

    class Meta:
        get_pk_field = "bundle_id"

    bundle_id = columns.UUID(primary_key=True)
    clip_id = columns.UUID(primary_key=True)
    position = columns.Integer(default=0)
    created_at = columns.TimeUUID(default=timeuuid_now)
    is_seen = columns.Boolean(default=False)
    media_type = columns.Text(default="reel")

    @property
    def json_data(self):
        return {
            "bundle_id": str(self.bundle_id),
            "clip_id": str(self.clip_id),
            "position": self.position,
            "media_type": self.media_type,
            "is_seen": self.is_seen,
        }


# ============================================================================
# Inbox Models for activity.api.views.inbox
# ============================================================================


class InboxThreadModel(DjangoCassandraModel):
    """
    Represents a Direct inbox thread.
    Models the thread structure in Instagram Direct.

    In production inbox:
    - Threads represent conversations between users
    - Threads have participants, messages, and read states
    - Thread metadata is cached in Direct cache for fast access
    """

    class Meta:
        get_pk_field = "thread_id"

    thread_id = columns.UUID(primary_key=True, default=uuid.uuid4)
    owner_id = columns.UUID(index=True)
    thread_type = columns.Text(default="private")  # private, group
    title = columns.Text()
    participant_ids = columns.List(columns.UUID)
    created_at = columns.TimeUUID(default=timeuuid_now)
    last_activity_at = columns.TimeUUID(default=timeuuid_now)
    is_muted = columns.Boolean(default=False)
    is_spam = columns.Boolean(default=False)
    unread_count = columns.Integer(default=0)
    last_seen_at = columns.TimeUUID()
    sequence_id = columns.BigInt(default=0)

    @property
    def published(self):
        return datetime_from_uuid1(self.created_at)

    @property
    def last_activity(self):
        return datetime_from_uuid1(self.last_activity_at)

    @property
    def json_data(self):
        return {
            "thread_id": str(self.thread_id),
            "owner_id": str(self.owner_id),
            "thread_type": self.thread_type,
            "title": self.title,
            "participant_ids": [str(pid) for pid in self.participant_ids]
            if self.participant_ids
            else [],
            "created_at": str(self.published),
            "last_activity_at": str(self.last_activity),
            "is_muted": self.is_muted,
            "is_spam": self.is_spam,
            "unread_count": self.unread_count,
        }


class InboxMessageModel(DjangoCassandraModel):
    """
    Represents a message in an inbox thread.
    Models the message structure in Instagram Direct.

    In production inbox:
    - Messages belong to threads
    - Messages have sender, content, and metadata
    - Message previews are shown in inbox view
    """

    class Meta:
        get_pk_field = "message_id"

    message_id = columns.UUID(primary_key=True, default=uuid.uuid4)
    thread_id = columns.UUID(index=True)
    sender_id = columns.UUID(index=True)
    message_type = columns.Text(default="text")  # text, media, link, voice, video_call
    text_content = columns.Text()
    media_id = columns.UUID()
    created_at = columns.TimeUUID(default=timeuuid_now)
    is_unsent = columns.Boolean(default=False)
    is_seen = columns.Boolean(default=False)
    reactions = columns.Map(columns.UUID, columns.Text)  # user_id -> reaction_type

    @property
    def timestamp(self):
        return datetime_from_uuid1(self.created_at)

    @property
    def json_data(self):
        return {
            "message_id": str(self.message_id),
            "thread_id": str(self.thread_id),
            "sender_id": str(self.sender_id),
            "message_type": self.message_type,
            "text_content": self.text_content,
            "created_at": str(self.timestamp),
            "is_unsent": self.is_unsent,
            "is_seen": self.is_seen,
        }


class InboxReadStateModel(DjangoCassandraModel):
    """
    Tracks read state for inbox threads.
    Models the read state management in Instagram Direct.

    In production inbox:
    - Read states track which messages have been seen
    - Used for badge count calculation
    - Cached for fast access
    """

    class Meta:
        get_pk_field = "user_id"

    user_id = columns.UUID(primary_key=True)
    thread_id = columns.UUID(primary_key=True)
    last_read_message_id = columns.UUID()
    last_read_at = columns.TimeUUID(default=timeuuid_now)
    unread_count = columns.Integer(default=0)

    @property
    def json_data(self):
        return {
            "user_id": str(self.user_id),
            "thread_id": str(self.thread_id),
            "last_read_message_id": str(self.last_read_message_id)
            if self.last_read_message_id
            else None,
            "unread_count": self.unread_count,
        }


class InboxPendingRequestModel(DjangoCassandraModel):
    """
    Tracks pending friend/message requests.
    Models the pending requests section in Instagram Direct inbox.

    In production inbox:
    - Pending requests appear as pinned items
    - Users can accept or decline requests
    """

    class Meta:
        get_pk_field = "user_id"

    user_id = columns.UUID(primary_key=True)
    requester_id = columns.UUID(primary_key=True)
    request_type = columns.Text(default="message")  # message, follow
    created_at = columns.TimeUUID(default=timeuuid_now)
    message_preview = columns.Text()
    is_spam = columns.Boolean(default=False)

    @property
    def json_data(self):
        return {
            "user_id": str(self.user_id),
            "requester_id": str(self.requester_id),
            "request_type": self.request_type,
            "message_preview": self.message_preview,
            "is_spam": self.is_spam,
        }

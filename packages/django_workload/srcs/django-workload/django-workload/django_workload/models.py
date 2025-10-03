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

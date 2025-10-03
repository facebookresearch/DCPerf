# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

import re
from itertools import chain
from operator import itemgetter

from django.core.cache import cache

from .models import FeedEntryModel, InboxEntryBase, InboxTypes, UserModel


class AbstractAggregator(object):
    def add(self, entry):
        pass

    def aggregate(self):
        pass


class Unaggregated(AbstractAggregator):
    def __init__(self):
        self.entries = []

    def add(self, entry):
        self.entries.append(entry.json_data)

    def aggregate(self):
        pass


class LikesAggregator(AbstractAggregator):
    def __init__(self):
        self.per_feedentry = {}

    def add(self, entry):
        self.per_feedentry.setdefault(entry.feedentryid, []).append(entry)

    def aggregate(self):
        feedentries = FeedEntryModel.objects.filter(id__in=list(self.per_feedentry))
        feedentry_by_id = {f.id: f for f in feedentries}
        user_by_id = {
            u.id: u
            for u in UserModel.objects.filter(
                id__in=list(
                    {
                        e.likerid
                        for entries in self.per_feedentry.values()
                        for e in entries
                    }
                )
            )
        }

        def describe(entries):
            users = [user_by_id[e.likerid].name for e in entries]
            if len(users) == 1:
                return "{} liked your post".format(users[0])
            elif len(users) == 2:
                return "{} and {} liked your post".format(*users)
            else:
                return "{}, {} and {} others liked your post".format(
                    users[0], users[1], len(users) - 2
                )

        self.entries = [
            {
                "type": "liked",
                "text": describe(entries),
                "published": str(feedentry_by_id[f].published),
            }
            for f, entries in self.per_feedentry.items()
        ]


class FollowersAggregator(AbstractAggregator):
    def __init__(self):
        self.userids = set()
        self.entries = []

    def add(self, entry):
        self.userids.add(entry.followerid)
        self.entries.append(entry)

    def aggregate(self):
        users = UserModel.objects.filter(id__in=list(self.userids))
        user_by_id = {u.id: u for u in users}

        self.entries = [
            {
                "type": "follower",
                "text": "{} started following you".format(
                    user_by_id[e.followerid].name
                ),
                "userid": e.followerid.hex,
                "published": str(e.published),
            }
            for e in self.entries
        ]


class Inbox(object):
    def __init__(self, request):
        self.request = request

    def load_inbox_entries(self):
        userid = self.request.user.id
        query = InboxEntryBase.objects.filter(userid=userid)
        # clear the _defer_fields entry to ensure we get full results;
        # if we don't only the base model fields are loaded.
        query._defer_fields.clear()
        return query

    def aggregate(self, entries):
        aggregators = {
            InboxTypes.COMMENT: [Unaggregated()],
            InboxTypes.LIKE: [LikesAggregator()],
            InboxTypes.FOLLOWER: [FollowersAggregator()],
        }
        for entry in entries:
            for aggregator in aggregators.get(entry.type, ()):
                aggregator.add(entry)

        for agg in chain.from_iterable(aggregators.values()):
            agg.aggregate()

        entries = chain.from_iterable(
            agg.entries for agg in chain.from_iterable(aggregators.values())
        )
        return sorted(entries, key=itemgetter("published"), reverse=True)

    def results(self):
        user = self.request.user
        key = "inbox.{}".format(user.id.hex)
        cached = cache.get(key)
        if cached is not None:
            return cached

        entries = self.load_inbox_entries()
        result = {"items": self.aggregate(entries)}
        cache.set(key, result, 24 * 60 * 60)  # 15
        return result

    def dup_data(self, item_list, conf):
        # duplicate the data
        while conf.loops < conf.mult_factor:
            conf.list_extend(item_list)
            conf.loops += 1
        return conf.get_list()

    def count_likes(self, item, conf):
        re_like = re.compile(conf.get_re_liked())
        re_follow = re.compile(conf.get_re_followed())
        if re_like.match(item["text"]):
            two_likes_re = re.compile(conf.get_two_likes())
            three_likes_re = re.compile(conf.get_three_likes())
            if three_likes_re.match(item["text"]) is not None:
                conf.fresh_likes += 3
            elif two_likes_re.match(item["text"]) is not None:
                conf.fresh_likes += 2
            else:
                conf.fresh_likes += 1
        elif re_follow.match(item["text"]):
            conf.fresh_followers += 1
        else:
            conf.other_items += 1

    def compute_stats_undup(self, item_list, conf):
        final_items = []
        for item in item_list:
            self.count_likes(item, conf)
            # un-duplicate the data
            exists = False
            for final_item in final_items:
                if final_item["published"] == item["published"]:
                    exists = True
                    break
            if not exists:
                final_items.append(item)
        return final_items

    def post_process(self, result):
        item_list = result["items"]
        conf = InboxConfig()

        new_list = self.dup_data(item_list, conf)
        final_items = self.compute_stats_undup(new_list, conf)
        conf.fresh_likes = int(conf.fresh_likes / conf.mult_factor)
        conf.fresh_followers = int(conf.fresh_followers / conf.mult_factor)
        conf.other_items = int(conf.other_items / conf.mult_factor)
        result["items"] = final_items
        result["summary"] = (
            "You have " + str(conf.fresh_likes) + " new "
            "likes, " + str(conf.fresh_followers) + " new "
            "followers and " + str(conf.other_items) + " "
            "other new items"
        )
        return result


class InboxConfig(object):
    def __init__(self):
        # Number of times the original inbox items list is duplicated in order
        # to make the view more Python intensive
        self.mult_factor = 700
        self.work_list = []
        self.re_liked = ".* liked .*"
        self.re_followed = ".* following .*"
        self.re_two_likes = ".* and .* liked your post"
        self.re_three_likes = ".*, .* and .* liked your post"
        self.fresh_likes = 0
        self.fresh_followers = 0
        self.other_items = 0
        self.loops = 0

    def list_extend(self, list_):
        self.work_list.extend(list_)

    def get_list(self):
        return self.work_list

    def get_re_liked(self):
        return self.re_liked

    def get_re_followed(self):
        return self.re_followed

    def get_two_likes(self):
        return self.re_two_likes

    def get_three_likes(self):
        return self.re_three_likes

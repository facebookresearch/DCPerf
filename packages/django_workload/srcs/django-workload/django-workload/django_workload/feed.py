# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

# Async can be introduced gradually. This file contains some async routines
# that are used from a synchronous endpoint.
import asyncio

from .models import FeedEntryModel, UserModel
from .users import suggested_users


def wait_for(coro):
    loop = asyncio.get_event_loop()
    return loop.run_until_complete(coro)


class Context(object):
    """Shared context among async methods"""

    def __init__(self, request):
        self.endresult = None
        self.prepared = None
        self.request = request
        self.user = self.request.user

    def result_for(self, step):
        return self.prepared.get(step, None)


class AsyncStep(object):
    def __init__(self, context):
        self.context = context

    async def prepare(self):
        """Do work that can be done in parallel"""
        pass

    @property
    def prepared_result(self):
        return self.context.result_for(self)

    def run(self):
        """Execute work in series; all prepare work has completed"""
        pass


class Feed(object):
    def __init__(self, request):
        self.request = request
        self.context = None

    def feed_page(self):
        self.prepare()
        self.run()
        result = self.post_process(self.context.endresult)
        return result

    def dup_data(self, item_list, config):
        # remove suggestions from items list
        items_len = len(item_list)
        for i in range(items_len - 1, -1, -1):
            if "entry" not in item_list[i]:
                config.sugg_list.append(item_list[i])
                item_list.pop(i)
        # duplicate the data
        for _ in range(config.get_mult_factor()):
            config.list_extend(item_list)

    def sort_data(self, config):
        # sort by comment count
        s_list = sorted(
            config.work_list, key=lambda x: x["entry"]["comment_count"], reverse=True
        )
        # inefficiently bubble sort by time stamp decreasingly
        while not config.is_sorted():
            items_len = len(s_list)
            config.swapped = False
            for i in range(items_len - 1):
                first = s_list[i]["entry"]["published"]
                second = s_list[i + 1]["entry"]["published"]
                if first < second:
                    aux = s_list[i]
                    s_list[i] = s_list[i + 1]
                    s_list[i + 1] = aux
                    config.swapped = True
            if not config.swapped:
                config.set_sorted(True)
        return s_list

    def post_process(self, result):
        item_list = result["items"]
        config = FeedConfig()

        self.dup_data(item_list, config)
        s_list = self.sort_data(config)
        # un-duplicate the data
        final_items = []
        for item in s_list:
            exists = False
            for final_item in final_items:
                if final_item["entry"]["pk"] == item["entry"]["pk"]:
                    exists = True
                    break
            if not exists:
                final_items.append(item)

        result["items"] = final_items
        result["items"].extend(config.sugg_list)
        return result

    def prepare(self):
        self.context = context = Context(self.request)
        self.steps = [
            FollowedEntries(context),
            SuggestedUsers(context),
            Assemble(context),
        ]
        self.context.prepared = dict(zip(self.steps, wait_for(self.async_prepare())))

    async def async_prepare(self):
        return await asyncio.gather(*(s.prepare() for s in self.steps))

    def run(self):
        for step in self.steps:
            step.run()


class FollowedEntries(AsyncStep):
    async def prepare(self):
        # The Cassandra ORM doesn't offer async support yet, so we'll use a
        # thread executor pool instead
        def fetch_10_posts(user):
            following = user.following
            return list(FeedEntryModel.objects.filter(userid__in=following).limit(10))

        def fetch_users(userids):
            return {u.id: u for u in UserModel.objects.filter(id__in=list(userids))}

        loop = asyncio.get_event_loop()
        entries = await loop.run_in_executor(None, fetch_10_posts, self.context.user)
        userids = {e.userid for e in entries}
        usermap = await loop.run_in_executor(None, fetch_users, userids)
        return (entries, usermap)

    def run(self):
        entries, usermap = self.prepared_result
        user_info = {id_: user.json_data for id_, user in usermap.items()}
        self.context.entries = [
            {
                "entry": {
                    "pk": str(e.id),
                    "comment_count": e.comment_count,
                    "published": e.published.timestamp(),
                    "user": user_info[e.userid],
                }
            }
            for e in entries
        ]


class SuggestedUsers(AsyncStep):
    async def prepare(self):
        def fetch_users(userids):
            return list(UserModel.objects.filter(id__in=userids))

        if len(self.context.user.following) < 25:
            # only suggest when this user isn't following so many people yet
            userids = suggested_users(self.context.user)
            loop = asyncio.get_event_loop()
            return await loop.run_in_executor(None, fetch_users, userids)

    def run(self):
        suggestions = self.prepared_result
        if suggestions:
            self.context.entries.insert(
                3, {"suggestions": [user.json_data for user in suggestions]}
            )


class Assemble(AsyncStep):
    def run(self):
        self.context.endresult = {
            "num_results": len(self.context.entries),
            "items": self.context.entries,
        }


class FeedConfig(object):
    def __init__(self):
        # Number of times the original items list is duplicated in order
        # to make the view more Python intensive
        self.mult_factor = 10
        self.sorted = False
        self.work_list = []
        self.sugg_list = []
        self.swapped = False

    def get_mult_factor(self):
        return self.mult_factor

    def is_sorted(self):
        return self.sorted

    def set_sorted(self, val):
        self.sorted = val

    def list_extend(self, list_):
        self.work_list.extend(list_)

# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

from .feed_flow.flow import FeedFlow


class FeedTimeline:
    """
    FeedTimeline using FeedFlow multi-step architecture.
    Now mimics IG Django's feed.api.views.timeline with FeedFlow orchestration.
    """

    def __init__(self, request):
        self.request = request
        self.feed_flow = FeedFlow(request)

    def get_timeline(self):
        """
        Main entry point - executes FeedFlow and returns timeline.
        Mimics IG's timeline() view which calls FeedFlow.next_page()
        """
        result = self.feed_flow.next_page()
        return result

    def post_process(self, result):
        """
        Legacy post-processing for compatibility.
        Adds additional CPU work to match original workload.
        """
        item_list = result.get("items", [])
        conf = FeedTimelineConfig()

        for _ in range(conf.mult_factor):
            conf.list_extend(item_list)

        sorted_list = sorted(
            conf.get_list(), key=lambda x: x.get("timestamp", 0), reverse=True
        )
        final_items = []

        for item in sorted_list:
            author = item.get("author", "unknown")
            conf.user = author
            conf.comments_total = conf.comments_total + item.get("comment_count", 0)
            conf.comments_per_user[conf.user] = item.get("comment_count", 0)

            exists = False
            for final_item in final_items:
                if final_item["id"] == item["id"]:
                    exists = True
                    break
            if not exists:
                final_items.append(item)

        result["comments_total"] = int(conf.comments_total / conf.mult_factor)
        result["items"] = final_items
        return result


class FeedTimelineConfig(object):
    def __init__(self):
        self.mult_factor = 5
        self.work_list = []
        self.user = ""
        self.comments_total = 0
        self.comments_per_user = {}

    def list_extend(self, list_):
        self.work_list.extend(list_)

    def get_list(self):
        return self.work_list

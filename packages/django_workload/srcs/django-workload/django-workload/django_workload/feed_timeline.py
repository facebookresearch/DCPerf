class FeedTimeline:
    def __init__(self, request):
        self.request = request

    def get_timeline(self):
        user = self.request.user
        feed = user.feed_entries().limit(20)
        user_info = user.json_data
        result = {
            "num_results": len(feed),
            "items": [
                {
                    "pk": str(e.id),
                    "comment_count": e.comment_count,
                    "published": e.published.timestamp(),
                    "user": user_info,
                }
                for e in feed
            ],
        }
        return result

    def post_process(self, result):
        item_list = result["items"]
        conf = FeedTimelineConfig()

        # duplicate the data
        for _ in range(conf.mult_factor):
            conf.list_extend(item_list)

        sorted_list = sorted(
            conf.get_list(), key=lambda x: x["published"], reverse=True
        )
        final_items = []

        for item in sorted_list:
            conf.user = item["user"]["name"]
            conf.comments_total = conf.comments_total + item["comment_count"]
            conf.comments_per_user[conf.user] = item["comment_count"]
            # un-duplicate the data
            exists = False
            for final_item in final_items:
                if final_item["pk"] == item["pk"]:
                    exists = True
                    break
            if not exists:
                final_items.append(item)

        result["comments_total"] = int(conf.comments_total / conf.mult_factor)
        result["items"] = final_items
        return result


class FeedTimelineConfig(object):
    def __init__(self):
        # Number of times the original items list is duplicated in order
        # to make the view more Python intensive
        self.mult_factor = 5
        self.work_list = []
        self.user = ""
        self.comments_total = 0
        self.comments_per_user = {}

    def list_extend(self, list_):
        self.work_list.extend(list_)

    def get_list(self):
        return self.work_list

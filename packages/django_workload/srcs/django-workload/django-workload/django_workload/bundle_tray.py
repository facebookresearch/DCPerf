import logging
import time

from django.core.cache import cache

from .models import BundleEntryModel, FeedEntryModel, UserModel

logger = logging.getLogger(__name__)


class BundleTray(object):
    def __init__(self, request):
        self.request = request

    def get_bundle(self):
        start_time = time.time()

        bundles = list(
            BundleEntryModel.objects.filter(
                userid__in=self.request.user.following
            ).limit(10)
        )
        logger.debug(
            "[perf] bundle_tray::bundle_entry.objects.filter: {}".format(
                time.time() - start_time
            )
        )

        # only one bundle per user
        userids = {}
        feedentryids = []
        for bundle in bundles:
            if bundle.userid in userids:
                continue
            userids[bundle.userid] = bundle.id
            feedentryids += bundle.entry_ids
        first_bundleids = set(userids.values())

        # Fetch user information
        start_time = time.time()
        userinfo = cache.get_many(list(userids))
        if userinfo is not None:
            missing_userinfo = [userid for userid in userids if userid not in userinfo]
            if missing_userinfo:
                for user in UserModel.objects.filter(id__in=missing_userinfo):
                    userinfo[user.id] = user.json_data

                cache.set_many({uid: userinfo[uid] for uid in missing_userinfo}, 60 * 5)
        else:
            for user in UserModel.objects.filter(id__in=list(userids)):
                userinfo[user.id] = user.json_data
            cache.set_many(userinfo, 60 * 5)
        logger.debug(
            "[perf] bundle_tray::user_model.objects.filter: {}".format(
                time.time() - start_time
            )
        )

        # fetch entry information
        feedentryinfo = {}
        start_time = time.time()
        feedentryinfo = cache.get_many(list(feedentryids))
        if feedentryinfo is not None:
            missing_feedentryinfo = [
                fid for fid in feedentryids if fid not in feedentryinfo
            ]
            if missing_feedentryinfo:
                for feedentry in FeedEntryModel.objects.filter(
                    id__in=missing_feedentryinfo
                ):
                    feedentryinfo[feedentry.id] = {
                        "pk": str(feedentry.id),
                        "comment_count": feedentry.comment_count,
                        "published": feedentry.published.timestamp(),
                    }
                cache.set_many(
                    {fid: feedentryinfo[fid] for fid in missing_feedentryinfo}
                )
        else:
            for feedentry in FeedEntryModel.objects.filter(id__in=list(feedentryids)):
                feedentryinfo[feedentry.id] = {
                    "pk": str(feedentry.id),
                    "comment_count": feedentry.comment_count,
                    "published": feedentry.published.timestamp(),
                }
            cache.set_many(feedentryinfo, 60 * 5)

        result = {
            "bundle": [
                {
                    "pk": str(b.id),
                    "comment_count": b.comment_count,
                    "published": b.published.timestamp(),
                    "user": userinfo[b.userid],
                    "items": [
                        feedentryinfo[f] for f in bundle.entry_ids if f in feedentryinfo
                    ],
                }
                for b in bundles
                if b.id in first_bundleids
            ]
        }
        logger.debug(
            "[perf] bundle_tray::feed_entry.objects.filter+bundle_process: {}".format(
                time.time() - start_time
            )
        )
        return result

    def dup_sort_data(self, bundle_list, conf):
        # duplicate the data
        for _ in range(conf.get_mult_factor()):
            conf.list_extend(bundle_list)
        sorted_list = sorted(
            conf.get_list(), key=lambda x: x["published"], reverse=True
        )
        conf.final_items = []
        return sorted_list

    def undup_data(self, item, conf):
        exists = False
        for final_item in conf.final_items:
            if final_item["published"] == item["published"]:
                exists = True
                break
        if not exists:
            conf.final_items.append(item)

    def post_process(self, res):
        bundle_list = res["bundle"]
        conf = BundleConfig()

        sorted_list = self.dup_sort_data(bundle_list, conf)
        for item in sorted_list:
            conf.comm_total = conf.comm_total + item["comment_count"]
            for sub in item["items"]:
                conf.comm_total = conf.comm_total + sub["comment_count"]
            # un-duplicate the data
            self.undup_data(item, conf)

        res["comments_total"] = int(conf.comm_total / conf.get_mult_factor())
        res["bundle"] = conf.final_items
        return res


class BundleConfig(object):
    def __init__(self):
        # Number of times the original bundle list is duplicated in order
        # to make the view more Python intensive
        self.mult_factor = 20
        self.comm_total = 0
        self.work_list = []
        self.final_items = []

    def get_mult_factor(self):
        return self.mult_factor

    def list_extend(self, list_):
        self.work_list.extend(list_)

    def get_list(self):
        return self.work_list

# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

import json
import os
import random
import uuid

from ctypes import CDLL

from cassandra.cqlengine.query import BatchQuery
from django.conf import settings

from django.core.cache import cache
from django.http import HttpResponse
from django.views.decorators.cache import cache_page
from django.views.decorators.http import require_http_methods
from django_statsd.clients import statsd

from .bundle_tray import BundleTray
from .feed import Feed
from .feed_timeline import FeedTimeline
from .inbox import Inbox
from .models import BundleSeenModel

from .users import require_user


# Used for sample-based profiling
SAMPLE_COUNT = 0

libib = CDLL("libicachebuster.so")

IB_MIN = int(os.environ.get("IB_MIN", 100000))
IB_MAX = int(os.environ.get("IB_MAX", 200000))


@cache_page(30)
def index(request):
    return HttpResponse("""\
<html><head><title>Welcome to the Django workload!</title></head>
<body>
<h1>Welcome to the Django workload!</h1>

<p>The following views are being tested</p>

<dl>
<dt><a href="/feed_timeline">feed_timeline</a></dt>
<dd>A simple per-user feed of entries in time</dd>

<dt><a href="/timeline">timeline</a></dt>
<dd>A ranked feed of entries from other users</dd>

<dt><a href="/bundle_tray">bundle_tray</a></dt>
<dd>A feed of current bundles, with nested content, from other users</dd>

<dt><a href="/inbox">inbox</a></dt>
<dd>The inbox view in a mobile app for the current user</dd>

<dt>/seen (POST only endpoint)</dt>
<dd>A view to increase counters and last-seen timestamps</dd>
</dl>

</body>
</html>""")


@require_user
def feed_timeline(request):
    # Produce a JSON response containing the 'timeline' for a given user
    libib.ibrun(random.randint(IB_MIN, IB_MAX))
    feed_timeline = FeedTimeline(request)
    result = feed_timeline.get_timeline()
    # sort by timestamp and do some more "meaningful" work
    result = feed_timeline.post_process(result)
    return HttpResponse(json.dumps(result), content_type="text/json")


@require_user
def timeline(request):
    # Produce a JSON response containing the feed of entries for a user
    libib.ibrun(random.randint(IB_MIN, IB_MAX))
    feed = Feed(request)
    result = feed.feed_page()
    return HttpResponse(json.dumps(result), content_type="text/json")


@require_user
def bundle_tray(request):
    # Fetch bundles of content from followers to show
    libib.ibrun(random.randint(IB_MIN, IB_MAX))
    bundle = BundleTray(request)
    result = bundle.get_bundle()
    result = bundle.post_process(result)
    return HttpResponse(json.dumps(result), content_type="text/json")


@require_user
def inbox(request):
    # produce an inbox from different sources of information
    libib.ibrun(random.randint(IB_MIN, IB_MAX))
    inbox = Inbox(request)
    result = inbox.results()
    result = inbox.post_process(result)
    return HttpResponse(json.dumps(result), content_type="text/json")


@require_http_methods(["POST"])
@require_user
def seen(request):
    # Record stats for items marked as seen on a mobile device
    # For workload purposes we ignore the posted data, and instead generate
    # some random data of our own, cached in memcached
    global SAMPLE_COUNT
    should_profile = False
    libib.ibrun(random.randint(IB_MIN, IB_MAX))

    if settings.PROFILING:
        SAMPLE_COUNT += 1
        if SAMPLE_COUNT >= settings.SAMPLE_RATE:
            SAMPLE_COUNT = 0
            should_profile = True

    bundleids = cache.get("bundleids")
    if bundleids is None:
        bundleids = [uuid.uuid4() for _ in range(1000)]
        cache.set("bundleids", bundleids, 24 * 60 * 60)
    entryids = cache.get("entryids")
    if entryids is None:
        entryids = [uuid.uuid4() for _ in range(10000)]
        cache.set("entryids", entryids, 24 * 60 * 60)

    with statsd.pipeline() as pipe, BatchQuery():
        for bundleid in random.sample(bundleids, random.randrange(3)):
            if should_profile:
                pipe.incr("workloadoutput.bundle.{}.seen".format(bundleid.hex))
            for entryid in random.sample(entryids, random.randrange(5)):
                if should_profile:
                    pipe.incr(
                        "workloadoutput.bundle.{}.{}.seen".format(
                            bundleid.hex, entryid.hex
                        )
                    )
                BundleSeenModel(
                    userid=request.user.id, bundleid=bundleid, entryid=entryid
                ).save()

    return HttpResponse(json.dumps({}), content_type="text/json")

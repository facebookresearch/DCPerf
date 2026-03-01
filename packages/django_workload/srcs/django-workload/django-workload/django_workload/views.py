# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

import json
import os

from django.http import HttpResponse
from django.views.decorators.cache import cache_page
from django.views.decorators.http import require_http_methods

from .bundle_tray import BundleTray
from .clips import Clips
from .feed import Feed
from .feed_timeline import FeedTimeline
from .inbox_handler import Inbox
from .seen_handler import SeenHandler
from .users import require_user


# Used for sample-based profiling
SAMPLE_COUNT = 0

# libib = CDLL("libicachebuster.so")

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
    # libib.ibrun(random.randint(IB_MIN, IB_MAX))
    feed_timeline = FeedTimeline(request)
    result = feed_timeline.get_timeline()
    # sort by timestamp and do some more "meaningful" work
    result = feed_timeline.post_process(result)
    return HttpResponse(json.dumps(result), content_type="text/json")


@require_user
def timeline(request):
    # Produce a JSON response containing the feed of entries for a user
    # libib.ibrun(random.randint(IB_MIN, IB_MAX))
    feed = Feed(request)
    result = feed.feed_page()
    return HttpResponse(json.dumps(result), content_type="text/json")


@require_user
def bundle_tray(request):
    # Fetch bundles of content from followers to show
    # libib.ibrun(random.randint(IB_MIN, IB_MAX))
    bundle = BundleTray(request)
    result = bundle.get_bundle()
    result = bundle.post_process(result)
    return HttpResponse(json.dumps(result), content_type="text/json")


@require_user
def inbox(request):
    # produce an inbox from different sources of information
    # libib.ibrun(random.randint(IB_MIN, IB_MAX))
    inbox = Inbox(request)
    result = inbox.results()
    result = inbox.post_process(result)
    return HttpResponse(json.dumps(result), content_type="text/json")


@require_http_methods(["GET", "POST"])
@require_user
def seen(request):
    """
    Mark entities as seen.

    Accepts both GET and POST methods.
    Optional parameters:
    - type: Entity type (bundle, inbox, clip, feed_timeline)
    - id: Entity UUID

    If no parameters provided, executes original random-sample logic.
    If parameters provided, marks the specific entity as seen.
    """
    handler = SeenHandler(request)
    result, status_code = handler.handle()
    return HttpResponse(
        json.dumps(result),
        content_type="text/json",
        status=status_code,
    )


@require_user
def clips(request):
    """
    Clips discovery endpoint.

    Models clips.api.views.async_stream_clips_discover from production IG Django.
    Returns a JSON response containing discovered clips/reels with ads blended in.
    """
    # libib.ibrun(random.randint(IB_MIN, IB_MAX))
    clips_handler = Clips(request)
    result = clips_handler.discover()
    result = clips_handler.post_process(result)

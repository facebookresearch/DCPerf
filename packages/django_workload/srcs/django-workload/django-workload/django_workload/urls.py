# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

from django.urls import re_path as url

from . import views


urlpatterns = [
    url(r"^$", views.index, name="index"),
    url(r"^feed_timeline$", views.feed_timeline, name="feed_timeline"),
    url(r"^timeline$", views.timeline, name="timeline"),
    url(r"^bundle_tray$", views.bundle_tray, name="bundle_tray"),
    url(r"^inbox$", views.inbox, name="inbox"),
    url(r"^seen$", views.seen, name="seen"),
]

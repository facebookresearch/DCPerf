#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import sys

endpoint_list = ["feed_timeline", "timeline", "bundle_tray", "inbox", "seen"]


def main(argv):
    num_endpoints = 1000
    if 1 < len(argv):
        num_endpoints = int(argv[1])

    with open("views.template") as fi:
        views_template = fi.read()
    template_list = []
    for endpoint in endpoint_list:
        with open(endpoint + ".template") as fi:
            template_list.append(fi.read())
    with open("views.py", "w") as fo:
        fo.write(views_template)
        for i in range(num_endpoints):
            for template in template_list:
                fo.write(template.replace("{IDX}", str(i)))

    with open("urls.py", "w") as fo:
        fo.write(
            """\
# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

from django.conf.urls import url

from . import views


urlpatterns = [
    url(r'^$', views.index, name='index'),
"""
        )
        for i in range(num_endpoints):
            fo.write(
                "    url(r'^feed_timeline{IDX}$', views.feed_timeline{IDX}, name='feed_timeline'),\n".replace(
                    "{IDX}", str(i)
                )
            )
            fo.write(
                "    url(r'^timeline{IDX}$', views.timeline{IDX}, name='timeline'),\n".replace(
                    "{IDX}", str(i)
                )
            )
            fo.write(
                "    url(r'^bundle_tray{IDX}$', views.bundle_tray{IDX}, name='bundle_tray'),\n".replace(
                    "{IDX}", str(i)
                )
            )
            fo.write(
                "    url(r'^inbox{IDX}$', views.inbox{IDX}, name='inbox'),\n".replace(
                    "{IDX}", str(i)
                )
            )
            fo.write(
                "    url(r'^seen{IDX}$', views.seen{IDX}, name='seen'),\n".replace(
                    "{IDX}", str(i)
                )
            )
        fo.write(
            """\
    url(r'^mlp$', views.mlp, name='mlp'),
]
"""
        )

    with open("urls_template.txt", "w") as fo:
        for i in range(num_endpoints):
            fo.write(f"http://localhost:8000/feed_timeline{i} 26\n")
            fo.write(f"http://localhost:8000/timeline{i} 25\n")
            fo.write(f"http://localhost:8000/bundle_tray{i} 25\n")
            fo.write(f"http://localhost:8000/inbox{i} 19\n")
            fo.write(f"http://localhost:8000/seen{i} POST <seen.json 5\n")


if "__main__" == __name__:
    main(sys.argv)

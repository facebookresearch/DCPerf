# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

# We want to be able to get at the request from various sections that are
# not passed in that request. Like when recording timings in memcached.
#
# Make the request object available as a thread-local. This is not as
# sophisticated as Flask's request stack, but should suffice here.
from threading import local


_requests = local()


def get_request():
    return getattr(_requests, "request", None)


def get_view_name(default="<unknown>"):
    request = get_request()
    if request is None:
        return default
    return getattr(request.resolver_match, "url_name", default)


class ThreadLocalRequest:
    def __init__(self, request):
        self.request = request

    def __enter__(self):
        _requests.request = self.request

    def __exit__(self, *exc_info):
        del _requests.request

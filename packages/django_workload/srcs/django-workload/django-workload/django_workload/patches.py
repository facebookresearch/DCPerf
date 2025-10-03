# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

from functools import wraps
from inspect import getdoc

from django.conf import settings
from statsd.client import StatsClient

from statsd.defaults import django as statsd_django_defaults

from .global_request import get_view_name

_patches = []

# Used for sample-based profiling
CASSANDRA_COUNT = 0
MEMCACHED_COUNT = 0


def register_patch(f):
    _patches.append((f, (getdoc(f) or "").partition("\n")[0]))


# TODO: send a pull request upstream
@register_patch
def patch_django_statsd_ipv6():
    """Make django_statsd work with IPv6"""

    def insert_ipv6(**kwargs):
        if "ipv6" not in kwargs:
            kwargs["ipv6"] = statsd_django_defaults.ipv6
        return StatsClient(**kwargs)

    # Only needs to be applied if STATSD_IPV6 is set to True and the connection
    # fails when trying to import the client.
    try:
        from django_statsd.clients import normal
    except OSError as e:
        if e.errno == -2:  # Name or service not known
            # patch the client to make sure we can support IPv6
            # Use sys.modules to avoid triggering the exception again
            import sys

            normal = sys.modules["django_statsd.clients.normal"]
            normal.StatsClient = insert_ipv6
        else:
            raise


if settings.PROFILING:

    @register_patch
    def patch_cassandra_execute():
        """Record timings for Cassandra operations"""
        from cassandra.cqlengine.query import AbstractQuerySet
        from django_statsd.clients import statsd

        def decorator(orig):
            @wraps(orig)
            def timed_execute(self, *args, **kwargs):
                global CASSANDRA_COUNT

                CASSANDRA_COUNT += 1
                if CASSANDRA_COUNT >= settings.SAMPLE_RATE:
                    CASSANDRA_COUNT = 0
                    key = "cassandra.{}.execute".format(get_view_name())
                    statsd.incr(key)
                    with statsd.timer(key):
                        return orig(self, *args, **kwargs)
                else:
                    return orig(self, *args, **kwargs)

            return timed_execute

        AbstractQuerySet._execute = decorator(AbstractQuerySet._execute)


@register_patch
def patch_cassandra_uwsgi_postfork():
    """Reset cassandra connection after forking.

    When running under uwsgi, use postfork to re-set the cassandra connection.
    Otherwise we run into issues with shared connections which time out.

    """
    try:
        from uwsgidecorators import postfork
    except ImportError:
        # Not available, presumably we are not running under uwsgi
        return
    from django_cassandra_engine.utils import get_cassandra_connections

    @postfork
    def reset_cassandra_connection():
        for _, conn in get_cassandra_connections():
            conn.reconnect()


if settings.PROFILING:

    @register_patch
    def patch_memcached_methods():
        """Record timings for the Memcached Django integration"""
        from django.core.cache.backends.memcached import BaseMemcachedCache
        from django_statsd.clients import statsd

        def decorator(orig):
            @wraps(orig)
            def timed(self, *args, **kwargs):
                global MEMCACHED_COUNT

                MEMCACHED_COUNT += 1
                if MEMCACHED_COUNT >= settings.SAMPLE_RATE:
                    MEMCACHED_COUNT = 0
                    key = "memcached.{}.{}".format(get_view_name(), orig.__name__)
                    with statsd.timer(key):
                        return orig(self, *args, **kwargs)
                else:
                    return orig(self, *args, **kwargs)

            return timed

        for name in (
            "add",
            "get",
            "set",
            "delete",
            "get_many",
            "incr",
            "decr",
            "set_many",
            "delete_many",
        ):
            orig = getattr(BaseMemcachedCache, name)
            setattr(BaseMemcachedCache, name, decorator(orig))


def apply():
    for patch, descr in _patches:
        print(descr)
        patch()

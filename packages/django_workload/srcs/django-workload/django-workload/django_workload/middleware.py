# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

# Used for sample-based profiling
SAMPLE_COUNT = 0


# We need access to request metadata from within patched support code. Store
# the request in a thread global
def global_request_middleware(get_response):
    from .global_request import ThreadLocalRequest

    def middleware(request):
        with ThreadLocalRequest(request):
            return get_response(request)

    return middleware


# Record memory and CPU stats per view
def memory_cpu_stats_middleware(get_response):
    import time

    import psutil
    from django.conf import settings
    from django_statsd.clients import statsd

    from .global_request import get_view_name

    mem_entries = (
        "rss",
        "shared_clean",
        "shared_dirty",
        "private_clean",
        "private_dirty",
    )

    def summed(info):
        res = dict.fromkeys(mem_entries, 0)
        for path_info in info:
            for name in mem_entries:
                res[name] += getattr(path_info, name)
        return res

    def middleware(request):
        global SAMPLE_COUNT

        SAMPLE_COUNT += 1
        if SAMPLE_COUNT >= settings.SAMPLE_RATE:
            SAMPLE_COUNT = 0
            cpu_before = time.clock_gettime(time.CLOCK_PROCESS_CPUTIME_ID)
            mem_before = summed(psutil.Process().memory_maps())
            try:
                return get_response(request)
            finally:
                cpu_after = time.clock_gettime(time.CLOCK_PROCESS_CPUTIME_ID)
                statsd.gauge("cpu.{}".format(get_view_name()), cpu_after - cpu_before)
                mem_after = summed(psutil.Process().memory_maps())
                mem_key_base = "memory.{}.{{}}".format(get_view_name())
                for name, after in mem_after.items():
                    diff = after - mem_before[name]
                    statsd.gauge(mem_key_base.format(name) + ".total", after)
                    statsd.gauge(mem_key_base.format(name) + ".change", diff)
        else:
            return get_response(request)

    return middleware

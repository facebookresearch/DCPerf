#!/usr/bin/env python3
# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

# Template file to configure the cluster.
# Copy this file to cluster_settings.py before starting the WSGI server
# and adjust as needed.
from django_workload.settings import *  # noqa F403

# Security settings
SECRET_KEY = "()2uyyko+p=dv*nmu$b5my9px!e0=6r5unm19or$02$-c62%gb"
DEBUG = False
ALLOWED_HOSTS = ["localhost", "ip6-localhost", "127.0.0.1", "::1"]

# Cassandra database
DATABASES["default"]["HOST"] = "localhost"

# Monitoring server
STATSD_HOST = "localhost"
STATSD_PORT = 8125

# Memcached connection
CACHES["default"]["LOCATION"] = "127.0.0.1:11811"

# Sample rate for profiling
SAMPLE_RATE = 1000

# Enable/disable profiling
PROFILING = False

if not PROFILING:
    MIDDLEWARE.remove("django_workload.middleware.memory_cpu_stats_middleware")
    MIDDLEWARE.remove("django_workload.middleware.GraphiteRequestTimingMiddleware")
    MIDDLEWARE.remove("django_workload.middleware.GraphiteMiddleware")

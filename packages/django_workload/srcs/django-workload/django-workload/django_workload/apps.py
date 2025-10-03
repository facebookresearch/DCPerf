# Copyright 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the license found in the
# LICENSE file in the root directory of this source tree.

from django.apps import AppConfig

from . import patches


class DjangoWorkloadConfig(AppConfig):
    name = "django_workload"
    verbose = "Django Workload"

    def ready(self):
        patches.apply()

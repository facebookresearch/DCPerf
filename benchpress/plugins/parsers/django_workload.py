#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.
import logging
import re

from benchpress.lib.baseline import BASELINES

from benchpress.lib.parser import Parser

logger = logging.getLogger(__name__)

ALPHANUMERIC_URL_REGEX = "^/[a-zA-Z0-9]*"
DJANGO_BASELINE = BASELINES["django"]
FAILED_EXEC_MSG = "FAILED EXEC: incorrect or faulty metrics!"


class DjangoWorkloadParser(Parser):
    """Example output:
    Running iteration 1 --- DONE
    WARNING: Got 12 HTTP codes different than 200
    Please see full Siege log in /tmp/siege_out_1

    Running iteration 2 --- DONE
    WARNING: Got 15 HTTP codes different than 200
    Please see full Siege log in /tmp/siege_out_2

    Running iteration 3 --- DONE
    WARNING: Got 39 HTTP codes different than 200
    Please see full Siege log in /tmp/siege_out_3

    Running iteration 4 --- DONE
    WARNING: Got 63 HTTP codes different than 200
    Please see full Siege log in /tmp/siege_out_4

    Running iteration 5 --- DONE
    WARNING: Got 34 HTTP codes different than 200
    Please see full Siege log in /tmp/siege_out_5

    Running iteration 6 --- DONE
    WARNING: Got 44 HTTP codes different than 200
    Please see full Siege log in /tmp/siege_out_6

    Running iteration 7 --- DONE
    WARNING: Got 65 HTTP codes different than 200
    Please see full Siege log in /tmp/siege_out_7


    Removing results with Transaction rate min=126.1 and max=321.0

    URL hit percentages:
    /seen:			6.70509841888%, expected 5%
    /inbox:			18.1104053374%, expected 19%
    /timeline:		25.7365801768%, expected 25%
    /feed_timeline:		23.0797435138%, expected 26%
    /bundle_tray:		26.3664189787%, expected 25%

    Transactions:				26059.8 hits ---- RSD 0.223504355592
    Availability:				99.83 % ---- RSD 0.00099365695824
    Elapsed time:				119.804 secs ---- RSD 0.000539913913686
    Data transferred:			51.362 MB ---- RSD 0.22504760529
    Response time:				0.646 secs ---- RSD 0.203864738491
    Transaction rate:			217.518 trans/sec ---- RSD 0.223460506355
    Throughput:				0.428 MB/sec ---- RSD 0.22037058173
    Concurrency:				133.842  ---- RSD 0.0677907822776
    Successful transactions:		26059.8  ---- RSD 0.223504355592
    Failed transactions:			39.4  ---- RSD 0.393720191465
    Longest transaction:			27.458  ---- RSD 0.311683103993
    Shortest transaction:			0.082  ---- RSD 0.0487804878049
    P50:					0.304 secs ---- RSD 0.0573539334676
    P90:					0.832 secs ---- RSD 0.404418093169
    P95:					1.478 secs ---- RSD 0.283535607117
    P99:					7.074 secs ---- RSD 0.428916627845

    Full Siege output is available in /tmp/siege_out_[N]
    """

    def parse_dw_data(self, data, metric):
        """Helper method to handle errors when extracting metrics and values"""

        try:
            num = float(data[0])
        except ValueError:
            num = -1.0
        if len(data) > 1:
            unit = data[1]
            metric += "_" + str(unit)

        return metric, num

    def parse_dw_key_val(self, line, dw_metrics):
        """Helper method to parse django-workload output into key-value data
        structure
        """

        line = line.split(":")
        metric = line[0].strip()
        data = line[1].strip()

        url_pattern = re.compile(ALPHANUMERIC_URL_REGEX)

        if url_pattern.match(metric) is not None:
            hit_percentage = float(data.split(",")[0][:-1])
            hit_percentage = round(hit_percentage, 3)
            url_key = "URL_hit_percentages_" + metric
            dw_metrics[url_key] = hit_percentage
        else:
            """Determine if metrics are faulty or invalid; notify service that
            benchmark execution has failed"""
            if "N/A" in data:
                dw_metrics[metric] = -1.0
                logger.error(FAILED_EXEC_MSG)
                return

            data = data.split("---")[0].strip().split(" ")
            metric, val = self.parse_dw_data(data, metric)
            dw_metrics[metric] = val

    def parse(self, stdout, stderr, returncode):
        """Parses the django-workload benchmark output to extract key metrics"""

        dw_metrics = {}
        has_url_hit_percentages = False

        for dw_line in stdout:
            # Skip empty or non-interpretable lines
            if not any(c.isalpha() for c in dw_line):
                continue

            if "URL hit percentage" in dw_line:
                has_url_hit_percentages = True
                continue

            if has_url_hit_percentages and ":" in dw_line:
                self.parse_dw_key_val(dw_line, dw_metrics)

        if "Transaction rate_trans/sec" in dw_metrics:
            rps = dw_metrics["Transaction rate_trans/sec"]
            dw_metrics["score"] = float(rps) / DJANGO_BASELINE

        return dw_metrics

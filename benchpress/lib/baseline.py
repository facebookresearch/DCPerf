#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

BASELINES = {
    "taobench": 683000.0,
    "feedsim": 57.0,
    "django": 958.0,
    "mediawiki": 1280.0,
    "sparkbench": 4.0,
}

JOB_TO_BM = {
    "oss_performance_mediawiki_mlp": "mediawiki",
    "django_workload_default": "django",
    "django_workload_arm": "django",
    "django_workload_custom": "django",
    "feedsim_default": "feedsim",
    "feedsim_autoscale": "feedsim",
    "spark_standalone_remote": "sparkbench",
    "tao_bench_64g": "taobench",
    "tao_bench_custom": "taobench",
    "tao_bench_autoscale": "taobench",
}


def get_raw_perf_metric(job_name, metrics):
    try:
        if JOB_TO_BM[job_name] == "taobench":
            return float(metrics["total_qps"])
        elif job_name == "feedsim_default":
            return float(metrics["final_achieved_qps"])
        elif job_name == "feedsim_autoscale":
            return float(metrics["overall"]["final_achieved_qps"])
        elif JOB_TO_BM[job_name] == "django":
            return float(metrics["Transaction rate_trans/sec"])
        elif JOB_TO_BM[job_name] == "mediawiki":
            return float(metrics["Combined"]["Siege RPS"])
        elif JOB_TO_BM[job_name] == "sparkbench":
            return 3600.0 / float(metrics["execution_time_test_93586"])
        else:
            return None
    except KeyError:
        return None


def get_score(job_name, metrics):
    raw_metric = get_raw_perf_metric(job_name, metrics)
    if raw_metric is None:
        return None

    try:
        bm_name = JOB_TO_BM[job_name]
    except KeyError:
        return None

    baseline = BASELINES[bm_name]
    return raw_metric / baseline

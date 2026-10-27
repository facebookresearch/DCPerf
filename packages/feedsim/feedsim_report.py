#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import csv
import json
import math
import re
from pathlib import Path

COUNTS = (
    "sent_requests",
    "successful_requests",
    "failed_requests",
    "pending_requests",
    "server_errors",
    "transport_errors",
)


def read_instance(log_path, csv_path, fixed_qps):
    log = Path(log_path).read_text()
    with Path(csv_path).open() as source:
        rows = list(csv.DictReader(source))
    if not rows:
        raise ValueError("No measurement rows")
    row = rows[-1]
    if any(not row.get(name) for name in (*COUNTS, "elapsed_secs")):
        raise ValueError("Missing request outcomes in driver measurement")
    metrics = {name: int(row[name]) for name in COUNTS}
    sent = metrics["sent_requests"]
    successful = metrics["successful_requests"]
    failed = metrics["failed_requests"]
    completed = successful + failed
    if sent != completed + metrics["pending_requests"]:
        raise ValueError("Request outcome counts do not reconcile")
    if failed != metrics["server_errors"] + metrics["transport_errors"]:
        raise ValueError("Failure counts do not reconcile")
    elapsed = float(row["elapsed_secs"])
    if not math.isfinite(elapsed) or elapsed <= 0:
        raise ValueError("Invalid measurement elapsed time")
    metrics.update(
        completed_requests=completed,
        elapsed_secs=elapsed,
        sent_qps=sent / elapsed,
        successful_qps=successful / elapsed,
        failed_qps=failed / elapsed,
        success_rate_pct=100 * successful / completed if completed else 0,
        failure_rate_pct=100 * failed / completed if completed else 0,
        completion_rate_pct=100 * completed / sent if sent else 0,
        pending_rate_pct=100 * metrics["pending_requests"] / sent if sent else 0,
    )
    percentile, target = "95p", None
    match = re.search(r"Searching for QPS where (\w+) latency <= ([0-9.]+)", log)
    if match:
        percentile, target = match[1], float(match[2])
    latency = float(row[percentile + "_ms"])
    if not math.isfinite(latency):
        latency = None
    flags = []
    if successful == 0:
        flags.append("no_successful_requests")
    if failed:
        flags.append("request_failures")
    if not fixed_qps and (target is None or latency is None or latency > target):
        flags.append("sla_failed")
    if "latency still could not converge" in log:
        flags.append("search_not_converged")
    metrics.update(
        final_requested_qps=float(row["requested_qps"]),
        final_achieved_qps=successful / elapsed,
        final_latency_msec=latency,
        benchmark_valid=not flags,
        validity_flags=flags,
        throughput_basis="successful completed requests / driver measurement elapsed time",
    )
    return metrics, percentile, target


def build_report(log_prefix, csv_prefix, instances, fixed_qps=False):
    report = {"target_percentile": "95p", "target_latency_msec": None}
    errors = []
    measured = []
    for index in range(1, instances + 1):
        try:
            result, percentile, target = read_instance(
                str(log_prefix) + str(index) + ".log",
                str(csv_prefix) + str(index) + ".txt",
                fixed_qps,
            )
        except (OSError, ValueError, KeyError) as error:
            errors.append(f"Instance {index}: {error}")
            continue
        report[str(index)] = result
        measured.append(result)
        report["target_percentile"] = percentile
        report["target_latency_msec"] = target
    overall = {name: sum(item[name] for item in measured) for name in COUNTS}
    for name in ("sent_qps", "successful_qps", "failed_qps", "final_requested_qps"):
        overall[name] = sum(item[name] for item in measured)
    completed = overall["successful_requests"] + overall["failed_requests"]
    overall.update(
        completed_requests=completed,
        final_achieved_qps=overall["successful_qps"],
        success_rate_pct=100 * overall["successful_requests"] / completed
        if completed
        else 0,
        failure_rate_pct=100 * overall["failed_requests"] / completed
        if completed
        else 0,
        completion_rate_pct=100 * completed / overall["sent_requests"]
        if overall["sent_requests"]
        else 0,
        pending_rate_pct=100 * overall["pending_requests"] / overall["sent_requests"]
        if overall["sent_requests"]
        else 0,
        average_latency_msec=sum(item["final_latency_msec"] for item in measured)
        / len(measured)
        if measured and all(item["final_latency_msec"] is not None for item in measured)
        else None,
    )
    valid = (
        not errors
        and len(measured) == instances
        and all(item["benchmark_valid"] for item in measured)
    )
    report.update(
        overall=overall,
        spawned_instances=instances,
        completed_instances=len(measured),
        successful_instances=sum(item["benchmark_valid"] for item in measured),
        benchmark_valid=valid,
        reporting_errors=errors,
        min_qps=min((item["successful_qps"] for item in measured), default=0),
        max_qps=max((item["successful_qps"] for item in measured), default=0),
        is_fixed_qps=int(fixed_qps),
    )
    return report


def public_metrics(report):
    metrics = {
        key: {
            name: item[name]
            for name in (
                "final_requested_qps",
                "final_achieved_qps",
                "final_latency_msec",
            )
        }
        for key, item in report.items()
        if key.isdigit()
    }
    metrics["overall"] = {
        name: report["overall"][name]
        for name in (
            "final_requested_qps",
            "final_achieved_qps",
            "average_latency_msec",
        )
    }
    metrics.update(
        target_percentile=report.get("target_percentile", ""),
        target_latency_msec=format(report["target_latency_msec"], "g")
        if report.get("target_latency_msec") is not None
        else "",
        spawned_instances=str(report["spawned_instances"]),
        successful_instances=report["completed_instances"],
        min_qps=report["min_qps"],
        max_qps=report["max_qps"],
        is_fixed_qps=report["is_fixed_qps"],
        dr_trace=report["dr_trace"],
    )
    return metrics


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log-prefix", required=True)
    parser.add_argument("--csv-prefix", required=True)
    parser.add_argument("--instances", type=int, required=True)
    parser.add_argument("--debug-log", type=Path)
    parser.add_argument("--fixed-qps", type=int, default=0)
    parser.add_argument("--dr-trace", type=int, default=0)
    parser.add_argument("--run-status", type=int, default=0)
    args = parser.parse_args()
    report = build_report(
        args.log_prefix,
        args.csv_prefix,
        args.instances,
        bool(args.fixed_qps),
    )
    report["dr_trace"] = args.dr_trace
    report["worker_exit_status"] = args.run_status
    report["benchmark_valid"] = report["benchmark_valid"] and args.run_status == 0
    debug_log = (
        args.debug_log or Path(args.log_prefix).parent / "feedsim_debug_metrics.log"
    )
    with debug_log.open("a") as output:
        output.write(json.dumps(report, allow_nan=False) + "\n")
    print(json.dumps(public_metrics(report), indent=4, allow_nan=False))
    if not report["benchmark_valid"]:
        return 2
    if report["min_qps"] < 0.8 * report["max_qps"]:
        maximum = max(
            item["final_requested_qps"] for key, item in report.items() if key.isdigit()
        )
        Path("/tmp/max_req_qps").write_text(str(math.ceil(maximum)))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import csv
import logging
import os
import re
import signal
import statistics
import subprocess
import threading
import time

from benchpress.lib import util
from benchpress.lib.hook import Hook


logger = logging.getLogger(__name__)

DEFAULT_PATH = ["pidstat"]
# -t : also show statistics for the threads of the selected processes
# -h : single-line, no headers per sample (easier to parse)
# Process selection (-p) is added dynamically based on the benchmark, see
# BENCHMARK_PROCESS_MAP below.
DEFAULT_OPTIONS = ["-t", "-h"]
# Sampling interval in seconds; pidstat runs indefinitely until terminated.
DEFAULT_INTERVAL = "5"

# Maps a benchmark name (job.benchmark_name) to the OS process name that does
# the actual work. The benchmark harness is rarely named the same as the
# process burning CPU, so we need this bridge to know what pidstat should
# target. Note: Linux truncates process comm names to 15 chars, so values must
# use the truncated form (e.g. tao_bench_serve).
BENCHMARK_PROCESS_MAP = {
    "oss_performance_mediawiki": "hhvm",
    "django_workload": "uwsgi",
    "tao_bench": "tao_bench_serve",
    "tao_bench_autoscale": "tao_bench_serve",
    "tao_bench_standalone": "tao_bench_serve",
    "feedsim": "LeafNodeRank",
    "feedsim_autoscale": "LeafNodeRank",
    "spark_standalone": "java",
    "video_transcode_bench": "ffmpeg",
}

# The benchmark process does not exist yet when before_job runs, so we poll
# for it in the background until it appears (or we give up).
PID_WAIT_TIMEOUT = 120  # seconds to wait for the benchmark process to appear
PID_POLL_INTERVAL = 1  # seconds between pgrep attempts
POLLER_STOP_TIMEOUT = 5  # seconds to wait for the poller thread to stop

MAX_TRIES = 5  # number of attempts to terminate pidstat process
PROCESS_TERMINATE_DELAY = 3  # seconds

# Per-thread metrics parsed from `pidstat -t` output. `breakdown_keys` must be a
# subset of these.
SUPPORTED_METRICS = ["CPU", "usr", "system", "guest", "wait"]
# Statistics computed per metric per timestamp bucket.
DEFAULT_STATS = ["avg"]


def normalize_thread_name(tname):
    """Strip numeric suffixes so threads are grouped by pool.

    Handles both 'srvIOThread24' -> 'srvIOThread' and 'dptworker_24' ->
    'dptworker'.
    """
    tname = re.sub(r"\d+$", "", tname)
    return tname.strip("_").strip("-").strip()


def _variance(values):
    """Population variance; 0.0 for fewer than two values (matches numpy.var)."""
    if len(values) < 2:
        return 0.0
    return statistics.pvariance(values)


_STAT_FUNCS = {
    "avg": statistics.fmean,
    "min": min,
    "max": max,
    "var": _variance,
}


def _format_stat(stat, values):
    func = _STAT_FUNCS.get(stat)
    if func is None:
        return ""
    return f"{func(values):.2f}"


def _parse_pidstat_log(log_path, normalize_thread_names):
    """Parse a raw `pidstat -t -h` log.

    Returns (threads, ordered_timestamps), where threads is
    {tname: {timestamp: {metric: [vals]}}} and ordered_timestamps preserves the
    order timestamps first appear in the log. Relying on file order (rather than
    parsing the time-of-day) keeps rows correctly ordered for runs that span
    midnight.
    """
    threads = {}
    # dict preserves insertion order and dedupes, acting as an ordered set.
    ordered_timestamps = {}
    with open(log_path, "r", encoding="utf-8") as f:
        for line in f:
            data = line.split()
            # AM/PM format (>=12 cols): "HH:MM:SS AM UID TGID TID ... Command"
            # 24h format    (>=11 cols): "HH:MM:SS    UID TGID TID ... Command"
            # Only per-thread lines (TGID="-", TID=<int>) carry usable data;
            # the parent process line has TID="-" and aggregate %CPU.
            if len(data) >= 12 and data[3] == "-" and data[4].isdigit():
                ts_offset = 2
            elif len(data) >= 11 and data[2] == "-" and data[3].isdigit():
                ts_offset = 1
            else:
                continue

            # Skip pidstat's terminal "Average:" rollup lines.
            if data[0].lower().startswith("average"):
                continue

            try:
                timestamp = " ".join(data[:ts_offset])
                usr = float(data[ts_offset + 3])
                system = float(data[ts_offset + 4])
                guest = float(data[ts_offset + 5])
                wait = float(data[ts_offset + 6])
                cpu = float(data[ts_offset + 7])
                tname = data[ts_offset + 9].replace("|__", "")
            except (ValueError, IndexError):
                continue

            if normalize_thread_names:
                tname = normalize_thread_name(tname)

            ordered_timestamps.setdefault(timestamp)
            bucket = threads.setdefault(tname, {}).setdefault(
                timestamp, {metric: [] for metric in SUPPORTED_METRICS}
            )
            bucket["usr"].append(usr)
            bucket["system"].append(system)
            bucket["guest"].append(guest)
            bucket["wait"].append(wait)
            bucket["CPU"].append(cpu)
    return threads, list(ordered_timestamps)


def _header_row(sorted_tnames, breakdown_keys, stats):
    header = ["Timestamp"]
    for tname in sorted_tnames:
        header.append(f"{tname}:count")
        for key in breakdown_keys:
            for stat in stats:
                header.append(f"{tname}:{key}.{stat}")
    return header


def _data_row(threads, timestamp, sorted_tnames, breakdown_keys, stats):
    row = [timestamp]
    empty_width = 1 + len(breakdown_keys) * len(stats)
    for tname in sorted_tnames:
        bucket = threads[tname].get(timestamp)
        if bucket is None:
            row.extend([""] * empty_width)
            continue
        row.append(len(bucket["CPU"]))
        for key in breakdown_keys:
            values = bucket[key]
            row.extend(_format_stat(stat, values) for stat in stats)
    return row


def postprocess_pidstat_log(
    log_path,
    csv_path,
    breakdown_keys=None,
    stats=None,
    normalize_thread_names=True,
):
    """Turn a raw pidstat log into a per-thread-pool CSV.

    Returns True if a CSV was written, False if the log had no thread data.
    """
    breakdown_keys = breakdown_keys or list(SUPPORTED_METRICS)
    stats = stats or list(DEFAULT_STATS)

    threads, ordered_timestamps = _parse_pidstat_log(log_path, normalize_thread_names)
    if not threads:
        logger.warning("pidstat: no thread data found in %s; skipping CSV", log_path)
        return False

    sorted_tnames = sorted(threads.keys())

    with open(csv_path, "w", newline="", encoding="utf-8") as csv_file:
        writer = csv.writer(csv_file)
        writer.writerow(_header_row(sorted_tnames, breakdown_keys, stats))
        for timestamp in ordered_timestamps:
            writer.writerow(
                _data_row(threads, timestamp, sorted_tnames, breakdown_keys, stats)
            )
    logger.info(
        "pidstat: wrote post-processed CSV (%d thread pools) to %s",
        len(sorted_tnames),
        csv_path,
    )
    return True


class CpuPidstat(Hook):
    """CpuPidstat collects per-process / per-thread CPU utilization in the
    background for the duration of the job.

    A background `pidstat` command is run at the configured poll interval
    (default 5 seconds). The raw log is written under the benchmark metrics
    directory for the job, and after_job post-processes it into a
    per-thread-pool CSV (avg/min/max/var per metric) alongside the log.

    Instead of monitoring every process (`-p ALL`), which is slow on hosts with
    many processes, the hook looks up the benchmark in BENCHMARK_PROCESS_MAP to
    find the process it should monitor, polls for that process to start, and
    then targets only its PIDs via `-p <pid1,pid2,...>`.

    Example hook:

    ```yaml
    hooks:
      - hook: cpu-pidstat
        options:
          interval: '1'      # sampling interval in seconds
          args:
            - '-t'           # per-thread stats
            - '-h'           # no per-sample headers
          # Post-processing (CSV) options:
          breakdown_keys: ['CPU', 'usr', 'system']  # subset of supported metrics
          stats: ['avg', 'max']                     # avg / min / max / var
          normalize_thread_names: true              # group threads by pool
    ```

    The sampling interval can be configured via the `interval` option (default
    5 seconds). If `args` explicitly contains a `-p` selection, the hook honors
    it and starts pidstat immediately without PID resolution. If the benchmark
    is not in BENCHMARK_PROCESS_MAP, or its process never appears, the hook
    falls back to `-p ALL`.
    """

    def __init__(self):
        self.background_process = None
        self.stdout = None
        self._poller_thread = None
        self._stop_event = None
        # Set in before_job; consumed by after_job's post-processing step.
        self.log_path = None
        self.breakdown_keys = list(SUPPORTED_METRICS)
        self.stats = list(DEFAULT_STATS)
        self.normalize_thread_names = True

    def before_job(self, opts, job):
        # Guard against instance reuse: if a prior job's resources are still
        # around (e.g. after_job failed or never ran), clean them up before
        # overwriting them, otherwise we leak the open file and orphan the
        # running pidstat process.
        if (
            self.background_process is not None
            or self.stdout is not None
            or self._poller_thread is not None
        ):
            logger.warning(
                "pidstat: before_job called with leftover state from a prior "
                "job; cleaning up before starting"
            )
            self.after_job(opts, job)

        opts = opts or {}
        args = list(opts.get("args", DEFAULT_OPTIONS))
        interval = str(opts.get("interval", DEFAULT_INTERVAL))

        # Post-processing config (consumed in after_job).
        self.breakdown_keys = list(opts.get("breakdown_keys", SUPPORTED_METRICS))
        unsupported = [
            key for key in self.breakdown_keys if key not in SUPPORTED_METRICS
        ]
        if unsupported:
            raise ValueError(
                f"Unsupported pidstat breakdown_keys {unsupported}; "
                f"supported metrics are {SUPPORTED_METRICS}"
            )
        self.stats = list(opts.get("stats", DEFAULT_STATS))
        unsupported_stats = [stat for stat in self.stats if stat not in _STAT_FUNCS]
        if unsupported_stats:
            raise ValueError(
                f"Unsupported pidstat stats {unsupported_stats}; "
                f"supported stats are {sorted(_STAT_FUNCS)}"
            )
        self.normalize_thread_names = opts.get("normalize_thread_names", True)

        metrics_dir = util.create_benchmark_metrics_dir(job.uuid)
        job_name = job.name.replace(" ", "_")
        iteration_num = job.iteration_num
        stdout_path = os.path.join(
            metrics_dir, f"{job_name}_{job.uuid}_{iteration_num}_pidstat.log"
        )
        # File descriptor closed in after_job(..)
        self.stdout = open(stdout_path, "w", encoding="utf-8")  # noqa P201
        self.log_path = stdout_path

        cmd_prefix = list(DEFAULT_PATH) + args

        # If the caller pinned process selection explicitly, honor it and start
        # pidstat right away (legacy behavior, no PID resolution).
        if "-p" in args:
            self._launch(self._with_interval(cmd_prefix, interval))
            return

        proc_name = BENCHMARK_PROCESS_MAP.get(job.benchmark_name)
        if proc_name is None:
            logger.warning(
                "pidstat: no process mapping for benchmark '%s'; "
                "falling back to '-p ALL'",
                job.benchmark_name,
            )
            self._launch(self._with_interval(cmd_prefix + ["-p", "ALL"], interval))
            return

        # The benchmark process is not running yet, so resolve its PIDs in the
        # background and launch pidstat once they are known.
        self._stop_event = threading.Event()
        self._poller_thread = threading.Thread(
            target=self._poll_and_launch,
            args=(cmd_prefix, proc_name, interval),
            name="pidstat-pid-poller",
            daemon=True,
        )
        self._poller_thread.start()

    def _poll_and_launch(self, cmd_prefix, proc_name, interval):
        """Wait for the benchmark process to start, then launch pidstat."""
        pids = []
        deadline = time.time() + PID_WAIT_TIMEOUT
        while not self._stop_event.is_set():
            pids = self._find_pids(proc_name)
            if pids or time.time() >= deadline:
                break
            self._stop_event.wait(PID_POLL_INTERVAL)

        # The job ended (or was torn down) before the process appeared.
        if self._stop_event.is_set():
            return

        if pids:
            selection = ["-p", ",".join(pids)]
            logger.info(
                "pidstat: monitoring '%s' (pids: %s)", proc_name, ",".join(pids)
            )
        else:
            selection = ["-p", "ALL"]
            logger.warning(
                "pidstat: process '%s' not found after %ds; falling back to '-p ALL'",
                proc_name,
                PID_WAIT_TIMEOUT,
            )
        self._launch(self._with_interval(cmd_prefix + selection, interval))

    @staticmethod
    def _find_pids(proc_name):
        """Return the list of PIDs whose process name matches proc_name."""
        try:
            result = subprocess.run(
                ["pgrep", proc_name],
                capture_output=True,
                text=True,
            )
        except OSError as e:
            logger.warning("pidstat: pgrep failed for '%s': %s", proc_name, e)
            return []
        return result.stdout.split()

    @staticmethod
    def _with_interval(cmd, interval):
        """Append the sampling interval as pidstat's final positional arg."""
        if cmd[-1].isdigit():
            return cmd
        return cmd + [interval]

    def _launch(self, cmd):
        logger.info("Starting background 'pidstat' hook: %s", " ".join(cmd))
        self.background_process = util.issue_background_command(
            cmd, self.stdout, self.stdout
        )

    def after_job(self, opts, job):
        # Stop the PID poller first so it does not launch pidstat while we are
        # tearing down.
        if self._stop_event is not None:
            self._stop_event.set()
        if self._poller_thread is not None:
            self._poller_thread.join(timeout=POLLER_STOP_TIMEOUT)

        # Stop process if it's still running.
        if self.background_process and self.background_process.poll() is None:
            # Send SIGINT first, to gracefully stop
            self.background_process.send_signal(signal.SIGINT)
            # Check if process has not finished
            exited_cleanly = False
            for _ in range(MAX_TRIES):
                time.sleep(PROCESS_TERMINATE_DELAY)
                # check if the process has exited
                if self.background_process.poll() is not None:
                    exited_cleanly = True
                    break

            if not exited_cleanly:
                # SIGINT was ignored; escalate to SIGTERM.
                self.background_process.terminate()
                try:
                    self.background_process.wait(timeout=PROCESS_TERMINATE_DELAY)
                except subprocess.TimeoutExpired:
                    # SIGTERM can be caught/ignored; SIGKILL cannot, so force
                    # kill as a last resort.
                    logger.warning(
                        "pidstat: process did not stop after SIGTERM; sending SIGKILL"
                    )
                    self.background_process.kill()

        # Close stdout file descriptor
        if self.stdout:
            self.stdout.close()

        self._postprocess_log()

        # Reset state so the instance can be safely reused for another job.
        self.background_process = None
        self.stdout = None
        self._poller_thread = None
        self._stop_event = None
        self.log_path = None

    def _postprocess_log(self):
        """Turn the raw pidstat log into a per-thread-pool CSV.

        Failures here must not break teardown, so IO errors are swallowed after
        logging.
        """
        if not self.log_path:
            return
        csv_path = os.path.splitext(self.log_path)[0] + ".csv"
        try:
            postprocess_pidstat_log(
                self.log_path,
                csv_path,
                breakdown_keys=self.breakdown_keys,
                stats=self.stats,
                normalize_thread_names=self.normalize_thread_names,
            )
        except OSError as e:
            logger.warning("pidstat: post-processing failed: %s", e)

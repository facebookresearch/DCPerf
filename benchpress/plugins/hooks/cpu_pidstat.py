# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe
import logging
import os
import signal
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


class CpuPidstat(Hook):
    """CpuPidstat collects per-process / per-thread CPU utilization in the
    background for the duration of the job.

    A background `pidstat` command is run at the configured poll interval
    (default 5 seconds). The raw log is written under the benchmark metrics
    directory for the job and can be post-processed offline (e.g. via
    `ai_codesign/nonprod/aashvi/pidstat_postprocessing.py`) to produce a
    per-thread-pool CSV.

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

        metrics_dir = util.create_benchmark_metrics_dir(job.uuid)
        job_name = job.name.replace(" ", "_")
        iteration_num = job.iteration_num
        stdout_path = os.path.join(
            metrics_dir, f"{job_name}_{job.uuid}_{iteration_num}_pidstat.log"
        )
        # File descriptor closed in after_job(..)
        self.stdout = open(stdout_path, "w", encoding="utf-8")  # noqa P201

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

        # Reset state so the instance can be safely reused for another job.
        self.background_process = None
        self.stdout = None
        self._poller_thread = None
        self._stop_event = None

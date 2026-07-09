#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import logging
import os
import threading
import traceback

from benchpress.lib import open_source
from benchpress.lib.hook import Hook
from benchpress.lib.util import get_artifacts_dir

from .perf_monitors import (
    cpufreq_cpuinfo,
    cpufreq_scaling,
    memstat,
    mpstat,
    netstat,
    perfstat,
    power,
    topdown,
    vmstat,
)

if not open_source:
    from .perf_monitors import fb_power

DEFAULT_OPTIONS = {
    "mpstat": {
        "interval": 5,
        "enabled": True,
    },
    "cpufreq_scaling": {
        "interval": 5,
        "enabled": True,
    },
    "cpufreq_cpuinfo": {
        "interval": 5,
        "enabled": True,
    },
    "perfstat": {"interval": 5, "additional_events": [], "enabled": True},
    "netstat": {"interval": 5, "additional_counters": [], "enabled": True},
    "memstat": {"interval": 5, "additional_counters": [], "enabled": True},
    "topdown": {"interval": 5, "enabled": True},
    "power": {"interval": 1, "enabled": True},
    "vmstat": {"interval": 5, "enabled": True},
}

if not open_source:
    DEFAULT_OPTIONS["fb_power"] = {
        "interval": 10,
        "post_process": True,
        "enabled": True,
    }

AVAIL_MONITORS = {
    "mpstat": mpstat.MPStat,
    "cpufreq_scaling": cpufreq_scaling.CPUFreq,
    "cpufreq_cpuinfo": cpufreq_cpuinfo.CPUFreq,
    "perfstat": perfstat.PerfStat,
    "netstat": netstat.NetStat,
    "memstat": memstat.MemStat,
    "topdown": topdown.TopDown,
    "power": power.Power,
    "vmstat": vmstat.VMStat,
}

if not open_source:
    # pyrefly: ignore [unbound-name]
    AVAIL_MONITORS["fb_power"] = fb_power.FBPower

logger = logging.getLogger(__name__)


# Env var used to advertise the stage FIFO path to a child benchmark process.
# When set, a stage-aware benchmark (e.g. WDL prod_set's run_prod.sh) writes
# stage markers like ``START <stage_name>`` and ``STOP`` to that FIFO so
# this hook can rotate the perf monitor set per sub-benchmark and emit one
# folder of perf data per stage.
PERF_STAGE_FIFO_ENV = "BENCHPRESS_PERF_STAGE_FIFO"


def _resolve_subdir_option(opts):
    """Return whether the user opted into stage-aware mode.

    Stage-aware mode is requested either by setting ``stage_aware: true`` in
    the perf hook options, or by leaving the default (False). Today the only
    benchmark that emits stage markers is WDL prod_set, but the wiring is
    generic.
    """
    if not isinstance(opts, dict):
        return False
    return bool(opts.get("stage_aware", False))


class Perf(Hook):
    def before_job(self, opts, job):
        self.opts = DEFAULT_OPTIONS
        for key in DEFAULT_OPTIONS.keys():
            if not isinstance(opts, dict):
                break
            if key in opts:
                self.opts[key].update(opts[key])

        self.benchmark_metrics_dir = os.path.join(
            get_artifacts_dir(), f"benchmark_metrics_{job.uuid}"
        )
        if not os.path.isdir(self.benchmark_metrics_dir):
            os.mkdir(self.benchmark_metrics_dir)

        self._stage_aware = _resolve_subdir_option(opts)
        self._stage_thread = None
        self._stage_fifo_path = None
        self._stage_lock = threading.Lock()
        self._current_stage = None
        self._current_monitors = []

        if self._stage_aware:
            self._start_stage_coordinator(job)
        else:
            # Legacy / default path: start one set of monitors that span the
            # entire benchmark.
            self.monitors = self._build_and_run_monitors(job, subdir=None)

    def after_job(self, opts, job):
        if self._stage_aware:
            self._stop_stage_coordinator()
            return
        for monitor in self.monitors:
            monitor.terminate()
        for monitor in self.monitors:
            monitor.write_csv()

    # ------------------------------------------------------------------
    # Stage-aware mode
    # ------------------------------------------------------------------

    def _start_stage_coordinator(self, job):
        """Open a FIFO under the benchmark metrics dir and spawn a thread
        that reads ``START <stage>`` / ``STOP`` commands. Each START gets
        a fresh monitor set whose CSVs land in
        ``benchmark_metrics_<uuid>/<stage>/``.

        The FIFO path is exposed via the ``BENCHPRESS_PERF_STAGE_FIFO``
        env var so the child benchmark process can find it. We deliberately
        DO NOT start any monitors yet -- the coordinator launches them
        when the first STAGE command arrives.
        """
        self._stage_fifo_path = os.path.join(
            self.benchmark_metrics_dir, "perf_stage.fifo"
        )
        # Recreate the FIFO each run so a stale one from a previous run
        # never confuses us.
        if os.path.exists(self._stage_fifo_path):
            os.unlink(self._stage_fifo_path)
        os.mkfifo(self._stage_fifo_path, 0o600)
        os.environ[PERF_STAGE_FIFO_ENV] = self._stage_fifo_path
        self._stage_stop_event = threading.Event()
        self._stage_thread = threading.Thread(
            target=self._stage_loop,
            args=(job,),
            name="perf-stage-coordinator",
            daemon=True,
        )
        self._stage_thread.start()
        logger.info(
            f"Perf hook: stage-aware mode active, FIFO at {self._stage_fifo_path}"
        )

    def _stop_stage_coordinator(self):
        # Tell the coordinator to exit and unblock the FIFO read by writing
        # a final STOP. A standalone writer also lets us flush any lingering
        # monitor set without depending on the benchmark script having sent
        # its own STOP.
        self._stage_stop_event.set()
        try:
            with open(self._stage_fifo_path, "w") as f:
                f.write("STOP\n")
                f.write("__EXIT__\n")
        except Exception as e:
            logger.warning(f"Perf hook: failed to nudge stage FIFO: {e}")
        if self._stage_thread is not None:
            self._stage_thread.join(timeout=30)
        # Clean up the FIFO and the env var so a subsequent benchmark in the
        # same process doesn't accidentally re-use stale state.
        try:
            os.unlink(self._stage_fifo_path)
        except OSError:
            pass
        os.environ.pop(PERF_STAGE_FIFO_ENV, None)

    def _stage_loop(self, job):
        """Read commands from the FIFO until __EXIT__. Each line is one of:

          START <stage_name>
          STOP
          __EXIT__

        START allocates fresh monitors and runs them; STOP terminates them
        and writes their CSVs. Multiple START/STOP cycles are supported.

        Implementation note: each writer that closes the FIFO causes EOF on
        the reader, so we have to re-open the FIFO after every burst rather
        than holding a single file handle. Each ``open(..., "r")`` blocks
        until a writer is available again -- which is exactly the behavior
        we want for a coordinator that's awaiting the next stage marker.
        """
        try:
            while not self._stage_stop_event.is_set():
                with open(self._stage_fifo_path, "r") as fifo:
                    for line in fifo:
                        line = line.strip()
                        if not line:
                            continue
                        if line == "__EXIT__":
                            self._end_current_stage(job)
                            return
                        if line.startswith("START "):
                            stage = line[len("START ") :].strip()
                            self._begin_stage(job, stage)
                            continue
                        if line == "STOP":
                            self._end_current_stage(job)
                            continue
                        logger.warning(
                            f"Perf hook: ignoring unknown stage command: {line!r}"
                        )
        except Exception as e:
            logger.warning(
                f"Perf hook: stage coordinator crashed: {type(e).__name__}: {e}"
            )

    def _begin_stage(self, job, stage_name):
        with self._stage_lock:
            if self._current_stage is not None:
                # Implicit stop of the previous stage -- the script forgot to
                # close it. Don't lose data; flush before starting the next.
                logger.warning(
                    f"Perf hook: implicit STOP of stage "
                    f"{self._current_stage!r} before START {stage_name!r}"
                )
                self._end_current_stage_locked(job)
            # Sanitize the stage name into a filesystem-friendly subdir.
            sanitized = _sanitize_subdir(stage_name)
            logger.info(f"Perf hook: starting stage {sanitized!r}")
            self._current_stage = sanitized
            self._current_monitors = self._build_and_run_monitors(job, subdir=sanitized)

    def _end_current_stage(self, job):
        with self._stage_lock:
            self._end_current_stage_locked(job)

    def _end_current_stage_locked(self, job):
        if self._current_stage is None:
            return
        logger.info(f"Perf hook: stopping stage {self._current_stage!r}")
        for monitor in self._current_monitors:
            try:
                monitor.terminate()
            except Exception as e:
                logger.warning(
                    f"Perf hook: terminating monitor {monitor.name} failed: {e}"
                )
        for monitor in self._current_monitors:
            try:
                monitor.write_csv()
            except Exception as e:
                logger.warning(f"Perf hook: write_csv on {monitor.name} failed: {e}")
        self._current_stage = None
        self._current_monitors = []

    # ------------------------------------------------------------------
    # Shared monitor setup
    # ------------------------------------------------------------------

    def _build_and_run_monitors(self, job, subdir):
        """Instantiate every enabled monitor (with the given subdir) and
        start it. Returns the list of started monitors.

        Refactored out of the original ``before_job`` body so both default
        mode and stage-aware mode share the same monitor-bring-up logic.
        """
        should_run_perf_stat = True
        monitors = []
        for mon_name in AVAIL_MONITORS.keys():
            # `enabled` is a perf-hook-level flag, not a monitor constructor
            # arg. Pop it out before passing the rest to the monitor class.
            init_args = dict(self.opts[mon_name])
            if not init_args.pop("enabled", True):
                logger.info(f"Perf monitor {mon_name} is disabled by config")
                continue
            try:
                MonitorClass = AVAIL_MONITORS[mon_name]
                monitor = MonitorClass(job_uuid=job.uuid, subdir=subdir, **init_args)
                # We should disable PerfStat (and not run anything that uses PMU)
                # if IntelPerfSpect3 is enabled.
                if isinstance(monitor, topdown.IntelPerfSpect3) and monitor.supported:
                    logger.info(
                        "Disabling PerfStat to avoid conflict with IntelPerfSpect3"
                    )
                    should_run_perf_stat = False
                monitors.append(monitor)
            except Exception as e:
                logger.warning(
                    f"Failed to load the perf monitor {mon_name} due to the following exception:"
                )
                logger.warning(traceback.print_exception(type(e), e, e.__traceback__))

        for monitor in monitors:
            try:
                if isinstance(monitor, perfstat.PerfStat) and not should_run_perf_stat:
                    continue
                monitor.run()
            except Exception as e:
                logger.warning(
                    f"Could not run perf monitor {monitor.name} due to the following exception:"
                )
                logger.warning(traceback.print_exception(type(e), e, e.__traceback__))
        return monitors


def _sanitize_subdir(name):
    """Make a stage name safe to use as a directory component.

    Strip path separators and trim. Keep alphanumerics, dashes, underscores;
    replace anything else with underscore.
    """
    out = []
    for ch in name.strip():
        if ch.isalnum() or ch in "-_.":
            out.append(ch)
        else:
            out.append("_")
    cleaned = "".join(out).strip("._")
    return cleaned or "unnamed_stage"

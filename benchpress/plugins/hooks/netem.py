#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

"""netem hook — apply tc/netem network impairments before a job, clear after.

Used by the dpu_perf suite's `rdma_lossy` benchmark to inject controlled
loss / delay on a RoCE link so we can characterise SACK / receiver-CC
behaviour. Generic enough to apply to any TCP/RDMA test that already
exposes its own server + client roles.

Job YAML:

    hooks:
      - hook: netem
        options:
          iface: eth0           # required; interface to qdisc on
          loss: "0.1%"          # optional; netem loss arg
          delay_us: 100         # optional; netem delay (microseconds)
          jitter_us: 10         # optional; netem delay jitter
          duplicate: ""         # optional; netem duplicate arg (e.g. "0.5%")
          corrupt: ""           # optional; netem corrupt arg

before_job() runs `tc qdisc add dev IFACE root netem ...`; after_job()
runs `tc qdisc del dev IFACE root` regardless of test outcome (uses
check=False so a missing qdisc on cleanup doesn't fail the run).

Requires root or CAP_NET_ADMIN.
"""

import logging
import subprocess

from benchpress.lib.hook import Hook


logger = logging.getLogger(__name__)


def _build_netem_args(opts):
    parts = []
    if "loss" in opts and opts["loss"]:
        parts += ["loss", str(opts["loss"])]
    if "delay_us" in opts and opts["delay_us"]:
        delay_arg = f"{opts['delay_us']}us"
        if "jitter_us" in opts and opts["jitter_us"]:
            delay_arg = f"{delay_arg} {opts['jitter_us']}us"
        parts += ["delay"] + delay_arg.split()
    if "duplicate" in opts and opts["duplicate"]:
        parts += ["duplicate", str(opts["duplicate"])]
    if "corrupt" in opts and opts["corrupt"]:
        parts += ["corrupt", str(opts["corrupt"])]
    return parts


class NetemHook(Hook):
    def before_job(self, opts, job=None):
        if "iface" not in opts:
            raise Exception("netem hook: 'iface' option is required")
        iface = opts["iface"]
        netem_args = _build_netem_args(opts)
        if not netem_args:
            logger.warning("netem hook: no impairment specified; skipping")
            self._iface = None
            return
        # Best-effort clean of any pre-existing qdisc so the `add` succeeds
        # even if a previous run was killed before its `after_job` ran.
        subprocess.run(
            ["tc", "qdisc", "del", "dev", iface, "root"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        cmd = ["tc", "qdisc", "add", "dev", iface, "root", "netem"] + netem_args
        logger.info("netem hook: %s", " ".join(cmd))
        subprocess.check_call(cmd)
        self._iface = iface

    def after_job(self, opts, job=None):
        iface = getattr(self, "_iface", None) or opts.get("iface")
        if not iface:
            return
        subprocess.run(
            ["tc", "qdisc", "del", "dev", iface, "root"],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )

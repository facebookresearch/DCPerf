#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import os
import subprocess
import tempfile
import unittest
from pathlib import Path


MODULE = Path(__file__).parent / "third_party/src/scripts/tuning_coordination.sh"


class TuningCoordinationTest(unittest.TestCase):
    def worker(self, directory, instance, body, timeout=30):
        env = dict(
            os.environ,
            FEEDSIM_COORDINATION_DIR=str(directory),
            FEEDSIM_INSTANCE_ID=str(instance),
            IS_AUTOSCALE_RUN="2",
            FEEDSIM_COORDINATION_TIMEOUT_SECS=str(timeout),
        )
        return subprocess.Popen(
            ["bash", "-c", 'source "$1"; coordination_init; ' + body, "_", str(MODULE)],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

    def test_finished_peer_keeps_loading_and_final_starts_together(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            driver = directory / "driver.py"
            driver.write_text(
                "import os,signal,time\n"
                "signal.signal(signal.SIGINT, lambda *_: exit(0))\n"
                "with open(os.environ['FEEDSIM_COORDINATION_DIR']+'/load.'+"
                "os.environ['FEEDSIM_INSTANCE_ID'], 'a', buffering=1) as f:\n"
                " while True:\n"
                "  f.write(str(time.time())+'\\n'); time.sleep(.05)\n"
            )
            common = """
                command="python3 $FEEDSIM_COORDINATION_DIR/driver.py"
                driver_thread_args() { echo ''; }
                cur_qps=600; adaptive_depth_arg=--depth=2
                output_csv_file="$FEEDSIM_COORDINATION_DIR/result.$FEEDSIM_INSTANCE_ID"
                for ((round=0; round<ROUNDS; round++)); do
                    $command & LOADTEST_PID=$!
                    coordination_probe_start
                    sleep .3
                    coordination_probe_end
                    coordination_stop_driver
                done
                coordination_finish_tuning
                python3 -c 'import time; print(time.time())' > "$FEEDSIM_COORDINATION_DIR/final.$FEEDSIM_INSTANCE_ID"
                COORD_COMPLETE=1
            """
            workers = [
                self.worker(directory, i, f"ROUNDS={rounds};" + common)
                for i, rounds in [(1, 1), (2, 4)]
            ]
            outputs = [worker.communicate(timeout=30) for worker in workers]
            for worker, output in zip(workers, outputs):
                self.assertEqual(worker.returncode, 0, output)
            finals = [float((directory / f"final.{i}").read_text()) for i in [1, 2]]
            self.assertLess(abs(finals[0] - finals[1]), 0.5)
            loads = [
                [float(x) for x in (directory / f"load.{i}").read_text().splitlines()]
                for i in [1, 2]
            ]
            # Bound the gap between samples, including the early finisher's hold.
            for samples in loads:
                self.assertLess(max(b - a for a, b in zip(samples, samples[1:])), 0.75)
            self.assertFalse(list(directory.glob("failed.*")))
            self.assertIn("hold_start", outputs[0][0])

    def test_peer_failure_releases_waiter(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            waiter = self.worker(directory, 1, "coordination_barrier test")
            failed = self.worker(directory, 2, "sleep .2; exit 7")
            failed.communicate(timeout=5)
            _, error = waiter.communicate(timeout=5)
            self.assertNotEqual(waiter.returncode, 0)
            self.assertIn("failed", error)

    def test_missing_peer_times_out(self):
        with tempfile.TemporaryDirectory() as temporary:
            waiter = self.worker(
                Path(temporary), 1, "coordination_barrier test", timeout=1
            )
            _, error = waiter.communicate(timeout=5)
            self.assertNotEqual(waiter.returncode, 0)
            self.assertIn("timed out", error)

    def test_single_instance_needs_no_coordinator(self):
        result = subprocess.run(
            [
                "bash",
                "-c",
                'source "$1"; IS_AUTOSCALE_RUN=1; coordination_init; coordination_probe_start; coordination_finish_tuning; echo "$COORD_ENABLED"',
                "_",
                str(MODULE),
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        self.assertEqual(result.stdout.strip(), "0")


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

import io
import math
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch

import dcperf_verify_fbgemm as verify


class VerifyFbgemmTest(unittest.TestCase):
    def test_metrics_required_for_each_binary(self):
        verify.validate_metrics("embedding_a_single", {"bandwidth": 1.2})
        verify.validate_metrics("fp16", {"CPU_A: FBP_t m=1 n=2 k=3": {"Gflops": 1.2}})
        verify.validate_metrics("spmdm8", {"embedding_spmdm8bit": [{"b/w_GBps": 1.2}]})
        for name in ("embedding_a_single", "fp16", "spmdm8"):
            with self.subTest(name=name), self.assertRaises(RuntimeError):
                verify.validate_metrics(name, {"unrelated": 1.2})

    def test_invalid_bandwidth(self):
        for value in (0, -1, math.nan, math.inf, "1.2", None):
            with self.subTest(value=value), self.assertRaises(RuntimeError):
                verify.validate_metrics("embedding_a_single", {"bandwidth": value})

    def test_native_invalid_metrics(self):
        for value in (0, -1, math.nan, math.inf):
            with self.subTest(value=value):
                with self.assertRaises(RuntimeError):
                    verify.validate_metrics("fp16", {"CPU_A: FBP_t": {"Gflops": value}})
                with self.assertRaises(RuntimeError):
                    verify.validate_metrics(
                        "spmdm8", {"embedding_spmdm8bit": [{"b/w_GBps": value}]}
                    )

    def test_process_exit_status_and_logs(self):
        with tempfile.TemporaryDirectory() as temporary, redirect_stdout(io.StringIO()):
            root = Path(temporary)
            runner = root / "run.sh"
            runner.write_text("#!/bin/bash\necho 'BW: 1.25 GB/s' >&2\n")
            verify.run_case(root, root, "embedding_a_single", ["unused"])
            self.assertIn(
                "BW: 1.25", (root / "embedding_a_single.stderr.log").read_text()
            )
            runner.write_text("#!/bin/bash\necho 'BW: 1.25 GB/s' >&2\nexit 7\n")
            with self.assertRaisesRegex(RuntimeError, "exited 7"):
                verify.run_case(root, root, "embedding_a_single", ["unused"])

    def test_empty_output_is_not_success(self):
        with tempfile.TemporaryDirectory() as temporary, redirect_stdout(io.StringIO()):
            root = Path(temporary)
            (root / "run.sh").write_text("#!/bin/bash\nexit 0\n")
            with self.assertRaisesRegex(RuntimeError, "missing"):
                verify.run_case(root, root, "embedding_a_single", ["unused"])

    def test_timeout_reports_partial_logs(self):
        with tempfile.TemporaryDirectory() as temporary, redirect_stdout(io.StringIO()):
            root = Path(temporary)
            (root / "run.sh").write_text(
                "#!/bin/bash\necho 'partial output'\necho 'partial error' >&2\nsleep 5\n"
            )
            with self.assertRaisesRegex(
                RuntimeError, "timed out after 0.1s\\npartial output\\npartial error"
            ):
                verify.run_case(root, root, "spmdm8", ["unused"], timeout=0.1)

    def test_missing_installed_binary(self):
        with tempfile.TemporaryDirectory() as temporary:
            with patch("sys.argv", ["verify", "--benchmark-root", temporary]):
                with self.assertRaisesRegex(
                    RuntimeError, "Missing installed executable"
                ):
                    verify.main()


if __name__ == "__main__":
    unittest.main()

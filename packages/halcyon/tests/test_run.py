#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

import argparse
import unittest
from unittest.mock import patch

import run as halcyon_run


class RunTest(unittest.TestCase):
    @patch("run.cleanup_directories")
    @patch("run.resolve_mounts")
    @patch("run.discover_mounts", return_value=[])
    @patch("run.parser")
    def test_partner_only_cleanup_does_not_require_local_mounts(
        self,
        parser,
        discover_mounts,
        resolve_mounts,
        cleanup_directories,
    ) -> None:
        parser.return_value.parse_args.return_value = argparse.Namespace(
            action="cleanup-filesets",
            mounts=None,
            partner="partner.example.com",
            partner_mounts="/mnt/hn1",
            ssh_command="ssh",
        )

        def resolve(value, host, ssh_command):
            if host is None:
                raise RuntimeError("no local mounts")
            return [value]

        resolve_mounts.side_effect = resolve

        try:
            returncode = halcyon_run.main()
        except RuntimeError as error:
            self.fail(f"partner-only cleanup required local mounts: {error}")

        self.assertEqual(returncode, 0)
        discover_mounts.assert_called_once_with(None, "ssh")
        resolve_mounts.assert_called_once_with("/mnt/hn1", "partner.example.com", "ssh")
        cleanup_directories.assert_called_once_with(
            ["/mnt/hn1"], "partner.example.com", "ssh"
        )

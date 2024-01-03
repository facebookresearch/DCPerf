#!/usr/bin/env python3
# Copyright (c) 2018-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

import json

from benchpress.lib.parser import Parser


class TaoBenchAutoscaleParser(Parser):
    def parse(self, stdout, stderr, returncode):
        """Extracts TAO bench results from stdout."""
        metrics = {}
        jsontext = ""
        met_json = False
        for line in stdout:
            if line.strip() == "{":
                met_json = True
            if met_json:
                jsontext += line
            if line.strip() == "}":
                break
        try:
            metrics = json.loads(jsontext)
        except Exception:
            pass

        return metrics

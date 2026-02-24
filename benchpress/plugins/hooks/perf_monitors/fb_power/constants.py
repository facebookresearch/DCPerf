#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

# mTLS certificate for BMC Redfish communication
DEFAULT_CERT_PATH = "/var/facebook/x509_identities/server.pem"

# Default collection interval (seconds)
DEFAULT_INTERVAL = 10

# Voltage Regulator efficiency factor (89%)
VR_EFFICIENCY = 0.89

# Chassis path templates by platform
CHASSIS_TEMPLATES = {
    "t1_trn": {
        "slot": "SENTINEL_DOME_SLOT_{slot}",
        "shared": [
            "Yosemite_4_Medusa_Board",
            "Yosemite_4_Fan_Board_0",
            "Yosemite_4_Fan_Board_1",
            "Yosemite_4_Spider_Board",
        ],
    },
    "t1_bgm": {"slot": "server{slot}", "shared": ["1"]},
    "t1_mln": {"slot": "server{slot}", "shared": ["1"]},
    "t1_cpl": {"slot": "server{slot}", "shared": ["1"]},
    "t11_grc_arm": {"slot": "server{slot}", "shared": ["1"]},
}

# LSST name patterns -> platform ID mapping
LSST_TO_PLATFORM = {
    "TRN": "t1_trn",
    "BGM": "t1_bgm",
    "MLN": "t1_mln",
    "CPL": "t1_cpl",
    "GRC": "t11_grc_arm",
}

# Case-insensitive patterns that indicate a sensor reports Watts
WATTS_UNITS = {"watts", "watt", "w"}

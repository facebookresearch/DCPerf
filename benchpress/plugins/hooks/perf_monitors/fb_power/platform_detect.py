#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# pyre-unsafe

import logging

from .constants import CHASSIS_TEMPLATES, LSST_TO_PLATFORM

logger = logging.getLogger(__name__)


def detect_platform(hostname: str) -> str:
    """Detect the platform type via LSST (Logical Server SubType) from Serf.

    Args:
        hostname: Full hostname (e.g., "rtptest8411.atn3.facebook.com")

    Returns:
        Platform ID string (e.g., "t1_bgm", "t1_trn").

    Raises:
        ValueError: If platform cannot be determined.
    """
    from ame.serf.clients.py.serf import Serf3ServiceClient
    from facebook.core_systems.logical_server_type.types import LogicalServerSubType
    from facebook.core_systems.queries.ttypes import Query

    with Serf3ServiceClient() as client:
        query = Query(whereMap={"name": hostname})
        devices = client.getDevices(
            query=query,
            columns=["logical_server_subtype"],
        )
        if not devices:
            raise ValueError(f"No device found in Serf for hostname {hostname}")

        lsst_id = devices[0].logical_server_subtype
        lsst = LogicalServerSubType(lsst_id)
        lsst_name = lsst.name

        logger.info(f"Detected LSST: {lsst_name} (id={lsst_id})")

        for pattern, platform_id in LSST_TO_PLATFORM.items():
            if pattern in lsst_name:
                logger.info(f"Mapped LSST {lsst_name} to platform {platform_id}")
                return platform_id

        raise ValueError(
            f"Unknown platform for LSST {lsst_name}. "
            f"Supported patterns: {list(LSST_TO_PLATFORM.keys())}"
        )


def get_chassis_paths(platform: str, slot: int) -> list[str]:
    """Return the Redfish chassis sensor paths for the given platform and slot.

    Args:
        platform: Platform ID (e.g., "t1_trn", "t1_bgm")
        slot: Server slot number

    Returns:
        List of Redfish sensor list URL paths. Includes the slot-specific
        chassis plus any shared chassis endpoints (e.g., fan boards, medusa).

    Raises:
        ValueError: If platform is unknown.
    """
    config = CHASSIS_TEMPLATES.get(platform)
    if not config:
        raise ValueError(
            f"Unknown platform: {platform}. "
            f"Supported platforms: {list(CHASSIS_TEMPLATES.keys())}"
        )
    slot_template = str(config["slot"])
    paths = [f"/redfish/v1/Chassis/{slot_template.format(slot=slot)}/Sensors"]
    for shared in config.get("shared", []):
        paths.append(f"/redfish/v1/Chassis/{shared}/Sensors")
    return paths

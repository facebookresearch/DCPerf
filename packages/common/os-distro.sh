#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

OS_DISTRO_ID="$(grep -oP '^ID=\"?\K\w+' /etc/os-release)"
OS_DISTRO_ID_LIKE="$(grep -oP '^ID_LIKE=\"?\K[a-z0-9 ]+' /etc/os-release || true)"


function get_os_distro_id() {
    echo "$OS_DISTRO_ID"
}

function get_os_distro_family() {
    echo "$OS_DISTRO_ID $OS_DISTRO_ID_LIKE"
}

function distro_is_like() {
    LIKE="$1"
    # shellcheck disable=SC2206
    OS_DISTROS=($OS_DISTRO_ID $OS_DISTRO_ID_LIKE)
    for DIST in "${OS_DISTROS[@]}"; do
        if [ "$LIKE" = "$DIST" ]; then
            return 0
        fi
    done
    return 1
}

#! /usr/bin/env bash

set -eou pipefail

SUDO=

if [ "$(id -u)" != "0" ]; then
    SUDO=sudo
fi

centos9() {
    echo "Installing dependencies for CentOS 9"

    $SUDO dnf install -y git python3-click python3-pyyaml python3-tabulate python3-pip xz-devel
    pip-3.9 install pandas

    $SUDO dnf install epel-release
    $SUDO dnf install 'dnf-command(config-manager)'
    $SUDO dnf config-manager --set-enabled crb
}

centos8() {
    echo "Installing dependencies for CentOS 8"

    echo ""
    echo "Since CentOS Stream 8 has reached EOL as of June 2024, some DCPerf's"
    echo "dependencies (such as folly) may start to drop its support. You may"
    echo "also encounter some troubles when trying to install packages via"
    echo "dnf. Therefore we recommend upgrading your OS to CentOS Stream 9. The"
    echo "newer version of folly may have begun to require newer versions of"
    echo "GCC compilers."
    echo

    read -n 1 -s -r -p  "Press any key to continue. "
    echo

    $SUDO dnf install -y python38 python38-pip git
    $SUDO alternatives --set python3 /usr/bin/python3.8
    pip-3.8 install click pyyaml tabulate pandas

    $SUDO dnf install epel-release
    $SUDO dnf install 'dnf-command(config-manager)'
    $SUDO dnf config-manager --set-enabled PowerTools

    $SUDO dnf install -y gcc-toolset-11
    scl enable gcc-toolset-11 bash
}

ubuntu22() {
    echo "Installing dependencies for Ubuntu 22.04"

    $SUDO apt update
    $SUDO apt install -y python3-pip git
    $SUDO pip3 install click pyyaml tabulate pandas
}

if [ -f /etc/os-release ]; then
    . /etc/os-release
else
    echo "Unsupported OS. Cannot determine OS ID from /etc/os-release"
    exit 1
fi

if [ "$ID" == "centos" ]; then
    if [ "$VERSION_ID" == "9" ]; then
        centos9
    elif [ "$VERSION_ID" == "8" ]; then
        centos8
    else
        echo "Unsupported CentOS version: $VERSION_ID"
        exit 1
    fi
elif [ "$ID" == "ubuntu" ]; then
    if [ "$VERSION_ID" == "22.04" ]; then
        ubuntu22
    else
        echo "Unsupported Ubuntu version: $VERSION_ID"
        exit 1
    fi
else
    echo "Unsupported OS: $ID"
    exit 1
fi

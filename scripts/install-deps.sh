#! /usr/bin/env bash

set -eou pipefail

SUDO=

if [ "$(id -u)" != "0" ]; then
    SUDO=sudo
fi

centos10() {
    echo "Installing dependencies for CentOS 10"

    $SUDO dnf install -y git python3-pyyaml python3-pip xz-devel lshw sysstat dmidecode
    pip-3.12 install click tabulate pandas packaging

    if ! [ "${IS_INTERNAL_TEST:-}" = "1" ]; then
        $SUDO dnf install -y epel-release numactl
        $SUDO dnf install -y 'dnf-command(config-manager)'
        $SUDO dnf config-manager --set-enabled crb
    fi
    # Remove perf first to avoid file conflict with kernel-headers
    # Both packages provide /usr/include/perf/perf_dlfilter.h
    $SUDO dnf remove -y perf || true
    # Exclude rpm-build to avoid rpm version conflicts between hsx.el10 and BaseOS
    # Also exclude el10 packages that conflict with installed hsx.el10 versions
    # Exclude rpm-sign packages which depend on conflicting rpm-libs
    # Note: kernel-headers is required by glibc-devel (which is required by gcc),
    # so we cannot exclude it.
    # Use --skip-broken to skip packages with broken dependencies
    $SUDO dnf group install -y "Development Tools" --exclude="texlive*" --exclude="rpm-build" --exclude="rpm-sign*" --exclude="rpm-4.19.1.1-23*" --exclude="rpm-libs-4.19.1.1-23*" --allowerasing --skip-broken
    # Reinstall perf to handle the file conflict with kernel-headers
    # Both packages provide /usr/include/perf/perf_dlfilter.h
    # Use --setopt=tsflags=replacefiles to allow overwriting files from kernel-headers
    $SUDO dnf install -y perf --setopt=tsflags=replacefiles || true

    $SUDO dnf install -y openssl-devel
}

centos9() {
    echo "Installing dependencies for CentOS 9"

    $SUDO dnf install -y git python3-click python3-pyyaml python3-tabulate python3-pip xz-devel lshw sysstat dmidecode
    pip-3.9 install pandas packaging

    # These are not necessary if it's run under internal test env
    if ! [ "${IS_INTERNAL_TEST:-}" = "1" ]; then
        $SUDO dnf install -y epel-release numactl
        $SUDO dnf install -y 'dnf-command(config-manager)'
        $SUDO dnf config-manager --set-enabled crb
    fi
    # Remove perf first to avoid file conflict with kernel-headers
    # Both packages provide /usr/include/perf/perf_dlfilter.h
    $SUDO dnf remove -y perf || true
    # Exclude rpm-build to avoid rpm version conflicts between hsx.el10 and BaseOS
    # Also exclude el10 packages to prevent repository metadata issues on Sandcastle
    # Exclude specific rpm versions that conflict with installed hsx.el10 packages
    # Exclude rpm-sign packages which depend on conflicting rpm-libs
    # Note: kernel-headers is required by glibc-devel (which is required by gcc),
    # so we cannot exclude it.
    # Use --skip-broken to skip packages with broken dependencies
    $SUDO dnf group install -y "Development Tools" --exclude="texlive*" --exclude="rpm-build" --exclude="rpm-sign*" --exclude="*.el10*" --exclude="rpm-4.19*" --exclude="rpm-libs-4.19*" --exclude="rpm-plugin-*4.19*" --allowerasing --skip-broken
    # Reinstall perf to handle the file conflict with kernel-headers
    # Both packages provide /usr/include/perf/perf_dlfilter.h
    # Use --setopt=tsflags=replacefiles to allow overwriting files from kernel-headers
    $SUDO dnf install -y perf --setopt=tsflags=replacefiles || true

    $SUDO dnf install -y openssl-devel
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

    $SUDO dnf install -y python38 python38-pip git lshw sysstat dmidecode numactl
    $SUDO alternatives --set python3 /usr/bin/python3.8
    pip-3.8 install click pyyaml tabulate pandas packaging

    $SUDO dnf install -y epel-release
    $SUDO dnf install -y 'dnf-command(config-manager)'
    $SUDO dnf config-manager --set-enabled PowerTools

    $SUDO dnf install -y gcc-toolset-11
    scl enable gcc-toolset-11 bash

    $SUDO dnf install -y openssl-devel
}

ubuntu22() {
    echo "Installing dependencies for Ubuntu 22.04"

    $SUDO apt update
    $SUDO apt install -y python3-pip git lshw sysstat dmidecode numactl
    $SUDO pip3 install click pyyaml tabulate pandas packaging

    $SUDO apt install -y libssl-dev
}

ubuntu24() {
    echo "Installing dependencies for Ubuntu 24.04"

    $SUDO apt update
    $SUDO apt install -y python3-pip git lshw sysstat dmidecode numactl
    $SUDO pip3 install --break-system-packages click pyyaml tabulate pandas packaging

    $SUDO apt install -y libssl-dev
}

if [ -f /etc/os-release ]; then
    . /etc/os-release
else
    echo "Unsupported OS. Cannot determine OS ID from /etc/os-release"
    exit 1
fi

if [ "$ID" == "centos" ]; then
    if [ "$VERSION_ID" == "10" ]; then
        centos10
    elif [ "$VERSION_ID" == "9" ]; then
        centos9
    elif [ "$VERSION_ID" == "8" ]; then
        centos8
    else
        echo "Unsupported CentOS version: $VERSION_ID"
        exit 1
    fi
elif [ "$ID" == "ubuntu" ]; then
    if [ "$VERSION_ID" == "24.04" ]; then
        ubuntu24
    elif [ "$VERSION_ID" == "22.04" ]; then
        ubuntu22
    else
        echo "Unsupported Ubuntu version: $VERSION_ID"
        exit 1
    fi
else
    echo "Unsupported OS: $ID"
    exit 1
fi

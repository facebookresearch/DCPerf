#!/bin/bash
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -eo pipefail

TEMPLATES_DIR="$(dirname "$(readlink -f "$0")")"
BENCHPRESS_ROOT="$(readlink -f "$TEMPLATES_DIR/../..")"
HHVM="/usr/local/hphpi/legacy/bin/hhvm"
HHVM_VERSION="3.30.12"
MARIADB_PWD="password"
MARIADB_DATADIR="/var/lib/mysql"
MARIADB_SOCKET="$MARIADB_DATADIR/mysql.sock"
MARIADB_ERRLOG="/var/log/mariadb-benchpress.err"
# Set to 1 when this run initialises the data directory from scratch (see
# start_mariadb_docker). Gates the empty-password root bootstrap below so a
# transient connect failure against an already-initialised DB does not trigger
# it.
MARIADB_FRESH_INIT=0
WRK_VERSION="4.2.0"
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"
OLD_LD_LIBRARY_PATH="$LD_LIBRARY_PATH"
export LD_LIBRARY_PATH="/opt/local/hhvm-3.30/lib:$LD_LIBRARY_PATH"

# Detects any container, not just Docker -- the name is kept for the callers.
# Tupperware runs LXC, which has no /.dockerenv. Keep this in sync with the
# copies in run.sh and cleanup_oss_performance_mediawiki.sh.
is_docker_container() {
  # Docker
  if [ -f /.dockerenv ]; then
    return 0
  fi
  # Podman / other OCI runtimes
  if [ -f /run/.containerenv ]; then
    return 0
  fi
  # Tupperware sets both of these in the task environment (container=lxc)
  if [ -n "${TUPD_CONTAINER:-}" ] || [ -n "${container:-}" ]; then
    return 0
  fi
  # Fallback: systemd's own detection covers lxc, docker, podman, ...
  if command -v systemd-detect-virt >/dev/null 2>&1; then
    if systemd-detect-virt --container --quiet; then
      return 0
    fi
  fi
  return 1
}

# Print whatever mariadbd recorded about its own failure. Called from every
# path that gives up on the server, so a failed start can no longer surface as
# a bare "can't connect through socket" with nothing attached.
mariadb_dump_errlog() {
  if [ -s "$MARIADB_ERRLOG" ]; then
    echo >&2 "--- last 50 lines of $MARIADB_ERRLOG ---"
    tail -n 50 "$MARIADB_ERRLOG" >&2
    echo >&2 "--- end of $MARIADB_ERRLOG ---"
  else
    echo >&2 "(mariadbd wrote nothing to $MARIADB_ERRLOG)"
  fi
}

# Function to start MariaDB in Docker container
start_mariadb_docker() {
  echo "Starting MariaDB in Docker container mode..."
  mkdir -p /run/mysqld
  chown mysql:mysql /run/mysqld
  chmod 755 /run/mysqld

  # A container image ships the mariadb package but not an initialised data
  # directory. On bare metal the systemd unit does this for us, which is why
  # only this branch needs it. Without it mariadbd creates its socket and then
  # exits because the system tables are missing, so the socket appears, every
  # connect is refused, and nothing says why.
  if [ ! -d "$MARIADB_DATADIR/mysql" ]; then
    echo "Initializing MariaDB data directory at $MARIADB_DATADIR..."
    mkdir -p "$MARIADB_DATADIR"
    chown mysql:mysql "$MARIADB_DATADIR"
    if command -v mariadb-install-db > /dev/null 2>&1; then
      mariadb-install-db --user=mysql --datadir="$MARIADB_DATADIR" > /dev/null
    else
      mysql_install_db --user=mysql --datadir="$MARIADB_DATADIR" > /dev/null
    fi
    MARIADB_FRESH_INIT=1
  fi

  # Give mariadbd an error log of its own. Its stderr is otherwise interleaved
  # with the installer's and routinely swallowed by the caller.
  : > "$MARIADB_ERRLOG"
  chown mysql:mysql "$MARIADB_ERRLOG"
  mariadbd --user=mysql --socket="$MARIADB_SOCKET" \
    --log-error="$MARIADB_ERRLOG" &
  mariadb_pid=$!

  echo "Waiting for MariaDB socket file to be created..."
  for i in {1..30}; do
    if [ -S "$MARIADB_SOCKET" ]; then
      echo "MariaDB socket file found, waiting for server to be ready..."
      sleep 5  # Give MariaDB additional time to initialize
      break
    fi
    echo "Waiting for MariaDB socket file (attempt $i/30)..."
    sleep 2
  done

  if ! [ -S "$MARIADB_SOCKET" ]; then
    echo >&2 "ERROR: MariaDB socket file not found after waiting."
    mariadb_dump_errlog
    exit 1
  fi

  # The socket existing is not proof the server is up: mariadbd creates it
  # before it finishes starting, and leaves it behind when it dies. Check the
  # daemon itself rather than trusting the inode.
  if ! kill -0 "$mariadb_pid" 2> /dev/null; then
    echo >&2 "ERROR: mariadbd exited during startup."
    mariadb_dump_errlog
    exit 1
  fi
}

# Function to start MariaDB on bare-metal machine
start_mariadb_systemctl() {
  echo "Starting MariaDB using systemctl..."
  systemctl start mariadb
}

# Function to restart MariaDB in Docker container
restart_mariadb_docker() {
  echo "Restarting MariaDB in Docker container mode..."
  pkill mariadb
  # Wait until MariaDB is fully killed before starting it again
  while pgrep -f mariadb > /dev/null; do
    sleep 1
  done
  # Mirror start_mariadb_docker: mariadbd running as --user=mysql cannot
  # create the log file when its directory is root-owned, so create and chown
  # it before pointing --log-error at it.
  : > "$MARIADB_ERRLOG"
  chown mysql:mysql "$MARIADB_ERRLOG"
  # Start MariaDB in the background with nohup to ensure it's fully detached
  nohup mariadbd --user=mysql --socket="$MARIADB_SOCKET" \
    --log-error="$MARIADB_ERRLOG" > /dev/null 2>&1 &
}

# Function to restart MariaDB on bare-metal machine
restart_mariadb_systemctl() {
  echo "Restarting MariaDB using systemctl..."
  systemctl restart mariadb
}

# Function to stop nginx based on environment
stop_nginx() {
  if is_docker_container; then
    echo "Stopping nginx in Docker container mode..."
    pkill nginx &
  else
    echo "Stopping nginx using systemctl..."
    systemctl stop nginx
  fi
}

# 0. Disable SELinux (required on CentOS 10 aarch64 for HHVM/nginx/MariaDB)
ARCH="$(uname -m)"
LINUX_VERSION_ID="$(awk -F "=" '/^VERSION_ID=/ {print $2}' /etc/os-release | tr -d '"')"
if [ "$LINUX_DIST_ID" = "centos" ] && [ "$ARCH" = "aarch64" ] && [[ "$LINUX_VERSION_ID" == 10* ]] && command -v getenforce &>/dev/null; then
  if [ "$(getenforce)" != "Disabled" ]; then
    echo "Disabling SELinux (required on CentOS 10 aarch64)..."
    sudo setenforce 0
    sudo sed -i 's/^SELINUX=enforcing/SELINUX=permissive/' /etc/selinux/config
  fi
fi

# 1. Install prerequisite packages
if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt install -y libevent-dev zlib1g zlib1g-dev
  apt install -y php-common php-cli php-dev
  apt install -y unzip
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf install -y libevent-devel zlib-devel
  dnf install -y php-common php-cli php-devel
  dnf install -y unzip bc
fi

# 2. Make sure hhvm 3.30.12 is installed
if ! which "$HHVM" >/dev/null 2>&1; then
  echo "Please install HHVM by the instruction first!"
  echo "If you have already installed, please link the hhvm executable to $HHVM."
  exit 1
fi

if [ "$($HHVM --version | head -n 1 | tr -d -c '0-9.')" != "$HHVM_VERSION" ]; then
  echo "oss-performance-mediawiki benchmark requires HHVM version $HHVM_VERSION."
  exit 1
fi

# 3. Install nginx
if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt install -y nginx
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf install -y nginx
fi
stop_nginx
# 4. Install mariadb
if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt install -y mariadb-server
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf install -y mariadb-server
fi

# Start MariaDB using appropriate method based on environment
if is_docker_container; then
  start_mariadb_docker
else
  start_mariadb_systemctl
fi

if ! [ -x "$(command -v mysql)" ]; then
  echo >&2 "Could not install mariadb!"
  exit 1
fi

# Set MySQL connection parameters based on environment
if is_docker_container; then
  MYSQL_SOCKET_PARAM="--socket=$MARIADB_SOCKET"
else
  MYSQL_SOCKET_PARAM=""
fi

# Try to connect multiple times with a delay between attempts
mysql_success=0
for i in {1..5}; do
  echo "Attempting to connect to MariaDB (attempt $i/5)..."
  if mysql -u root --password="$MARIADB_PWD" $MYSQL_SOCKET_PARAM -e ";" 2>/dev/null; then
    echo "Successfully connected to MariaDB!"
    mysql_success=1
    break
  fi
  if [ "$i" -eq 5 ]; then
    echo "Failed to connect to MariaDB after multiple attempts."
  else
    echo "Connection failed, waiting before retry..."
    sleep 5
  fi
done

# A freshly initialised data directory has no root password, so the loop above
# is expected to fail on a first install and this sets it. (An earlier version
# read `mysql_success=$?` off the completed for-loop instead of mysql, so the
# guard was always false and the bootstrap never ran.)
#
# Only bootstrap when this run initialised the data directory. If the loop
# failed against an already-initialised, password-protected DB the failure is
# transient, and an empty-password connect would just be refused -- running the
# bootstrap there would abort an otherwise-healthy install.
if [ "$mysql_success" -ne 1 ] && [ "$MARIADB_FRESH_INIT" = "1" ]; then
  echo "Bootstrapping the MariaDB root password..."
  if mysql -uroot --password="" $MYSQL_SOCKET_PARAM < "${TEMPLATES_DIR}/update_mariadb_pwd.sql" && \
     mysql -u root --password="$MARIADB_PWD" $MYSQL_SOCKET_PARAM -e ";" 2>/dev/null; then
    mysql_success=1
  else
    echo >&2 "ERROR: could not set the MariaDB root password."
    mariadb_dump_errlog
    exit 1
  fi
fi

# Anything still unconnected here is a genuine failure -- a transient outage
# against an initialised DB, or a bootstrap that already exited above. Fail
# loudly with the server log attached instead of running the grant below into
# a bare connect error.
if [ "$mysql_success" -ne 1 ]; then
  echo >&2 "ERROR: could not connect to MariaDB with the configured root password."
  mariadb_dump_errlog
  exit 1
fi

mysql -u root --password="$MARIADB_PWD" $MYSQL_SOCKET_PARAM < "${TEMPLATES_DIR}/grant_privileges.sql"

# 5. Install Siege
if ! [ -x "$(command -v siege)" ]; then
  rm -rf siege
  # shellcheck disable=SC2046
  git clone https://github.com/JoeDog/siege.git
  cd siege || exit 1
  # shellcheck disable=SC2046
  git checkout tags/v4.0.7
  ./utils/bootstrap
  automake --add-missing
  ./configure
  make -j8
  sudo make uninstall
  sudo make install
  cd ..
fi

# Copy siege.conf
mkdir -p "$HOME/.siege"
cp "${TEMPLATES_DIR}/siege.conf" "$HOME/.siege/siege.conf"
chmod 644 "$HOME/.siege/siege.conf"

# 6. Install wrk
pushd "${BENCHPRESS_ROOT}/benchmarks/oss_performance_mediawiki" || exit 1
if ! [ -d wrk ]; then
  git clone --branch "${WRK_VERSION}" https://github.com/wg/wrk
  pushd wrk || exit 1
  git apply --check "${TEMPLATES_DIR}/0004-wrk.diff" && \
    git apply "${TEMPLATES_DIR}/0004-wrk.diff"
  make
  popd # wrk
fi
popd # "${BENCHPRESS_ROOT}/benchmarks/oss_performance_mediawiki"

# 7. Install memcache
if ! [ -d memcached-1.5.12 ]; then
  git clone --branch 1.5.12 https://github.com/memcached/memcached memcached-1.5.12
  cd memcached-1.5.12 || { exit 1; }
  git apply --check "${TEMPLATES_DIR}/0002-memcached-centos9-compat.diff" && \
    git apply "${TEMPLATES_DIR}/0002-memcached-centos9-compat.diff"
  git apply --check "${TEMPLATES_DIR}/0003-memcached-signal.diff" && \
    git apply "${TEMPLATES_DIR}/0003-memcached-signal.diff"
  ./autogen.sh
  ./configure --prefix=/usr/local/memcached
  make -j8
  make install
  cd ..
fi

# 8. Installing OSS-performance

# shellcheck disable=SC2046
git clone https://github.com/hhvm/oss-performance.git
cd oss-performance || exit 1

# apply scale-out patch
# templates/oss-performance-mediawiki/0001-oss-performance-scalable-hhvm.diff
git apply --check "${TEMPLATES_DIR}/0001-oss-performance-scalable-hhvm.diff" \
    && git apply "${TEMPLATES_DIR}/0001-oss-performance-scalable-hhvm.diff"
# apply reuse-mediawiki patch
# templates/oss-performance-mediawiki/0006-oss-performance-reuse-mediawiki-hhvm.diff
git apply --check "${TEMPLATES_DIR}/0006-oss-performance-reuse-mediawiki-hhvm.diff" \
    && git apply "${TEMPLATES_DIR}/0006-oss-performance-reuse-mediawiki-hhvm.diff"
# Stream the nginx access log line-by-line (and strip null bytes) so metrics
# collection does not overflow HHVM's ~2GB single-string limit on
# high-throughput ARM hosts
git apply --check "${TEMPLATES_DIR}/0008-nginx-stream-access-log.diff" \
    && git apply "${TEMPLATES_DIR}/0008-nginx-stream-access-log.diff"

# apply options for mediawiki mini patch
git apply --check "${TEMPLATES_DIR}/0007-oss-performance-more-warmup-options.diff" \
    && git apply "${TEMPLATES_DIR}/0007-oss-performance-more-warmup-options.diff"

git apply --check "${TEMPLATES_DIR}/0005-scale-out-memcached.diff" \
    && git apply "${TEMPLATES_DIR}/0005-scale-out-memcached.diff"

# Copy wrk related stuff
cp "${TEMPLATES_DIR}/Wrk.php" ./base/Wrk.php
cp "${TEMPLATES_DIR}/WrkStats.php" ./base/WrkStats.php
cp "${TEMPLATES_DIR}/multi-request-txt.lua" ./scripts/multi-request-txt.lua.template

# shellcheck disable=SC2046
curl -O https://getcomposer.org/installer
mv installer composer-setup.php
export LD_LIBRARY_PATH="$OLD_LD_LIBRARY_PATH"
php composer-setup.php --2.2
export LD_LIBRARY_PATH="/opt/local/hhvm-3.30/lib:$LD_LIBRARY_PATH"
yes | $HHVM composer.phar install || true

# 9. Basic tuning
echo 1 | sudo tee /proc/sys/net/ipv4/tcp_tw_reuse

# 10. MariaDB tuning
sudo cp "${TEMPLATES_DIR}/my.cnf" "/etc/my.cnf"
if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  sudo mkdir -p /etc/my.cnf.d
fi

# Restart MariaDB using appropriate method based on environment
if is_docker_container; then
  restart_mariadb_docker
else
  restart_mariadb_systemctl
fi

# Ensure the script exits cleanly
echo "Installation completed successfully. MariaDB restarted with new configuration."
exit 0

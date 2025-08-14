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
WRK_VERSION="4.2.0"
LINUX_DIST_ID="$(awk -F "=" '/^ID=/ {print $2}' /etc/os-release | tr -d '"')"
OLD_LD_LIBRARY_PATH="$LD_LIBRARY_PATH"
export LD_LIBRARY_PATH="/opt/local/hhvm-3.30/lib:$LD_LIBRARY_PATH"

# Function to detect if running in Docker container
is_docker_container() {
  # Check for .dockerenv file
  if [ -f /.dockerenv ]; then
    return 0
  fi
  return 1
}

# Function to start MariaDB in Docker container
start_mariadb_docker() {
  echo "Starting MariaDB in Docker container mode..."
  mkdir -p /run/mysqld
  chown mysql:mysql /run/mysqld
  chmod 755 /run/mysqld

  mariadbd --user=mysql --socket=/var/lib/mysql/mysql.sock &

  while ! pgrep -f mysql > /dev/null; do
    echo "Waiting for MariaDB to start..."
    sleep 1
  done

  # Wait for the socket file to be created and MariaDB to be ready
  echo "Waiting for MariaDB socket file to be created..."
  for i in {1..30}; do
    if [ -S /var/lib/mysql/mysql.sock ]; then
      echo "MariaDB socket file found, waiting for server to be ready..."
      sleep 5  # Give MariaDB additional time to initialize
      break
    fi
    echo "Waiting for MariaDB socket file (attempt $i/30)..."
    sleep 2
  done

  if ! [ -S /var/lib/mysql/mysql.sock ]; then
    echo "ERROR: MariaDB socket file not found after waiting. Check MariaDB logs for errors."
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
  # Start MariaDB in the background with nohup to ensure it's fully detached
  nohup mariadbd --user=mysql --socket=/var/lib/mysql/mysql.sock > /dev/null 2>&1 &
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

# 1. Install prerequisite packages
if [ "$LINUX_DIST_ID" = "ubuntu" ]; then
  apt install -y libevent-dev zlib1g zlib1g-dev
  apt install -y php-common php-cli php-dev
  apt install -y unzip
elif [ "$LINUX_DIST_ID" = "centos" ]; then
  dnf install -y libevent-devel zlib-devel
  dnf install -y php-common php-cli php-devel
  dnf install -y unzip
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
  MYSQL_SOCKET_PARAM="--socket=/var/lib/mysql/mysql.sock"
else
  MYSQL_SOCKET_PARAM=""
fi

# Try to connect multiple times with a delay between attempts
for i in {1..5}; do
  echo "Attempting to connect to MariaDB (attempt $i/5)..."
  if mysql -u root --password="$MARIADB_PWD" $MYSQL_SOCKET_PARAM -e ";" 2>/dev/null; then
    echo "Successfully connected to MariaDB!"
    break
  fi
  if [ "$i" -eq 5 ]; then
    echo "Failed to connect to MariaDB after multiple attempts."
  else
    echo "Connection failed, waiting before retry..."
    sleep 5
  fi
done
mysql_success=$?
if [ "$mysql_success" -ne 0 ]; then
  mysql -uroot --password="" $MYSQL_SOCKET_PARAM < "${TEMPLATES_DIR}/update_mariadb_pwd.sql"
fi

mysql -u root --password="$MARIADB_PWD" $MYSQL_SOCKET_PARAM < "${TEMPLATES_DIR}/grant_privileges.sql"

# 5. Install Siege
if ! [ -x "$(command -v siege)" ]; then
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

#!/bin/bash

# set -e

TEMPLATES_DIR="$(dirname "$(readlink -f "$0")")"
HHVM="/usr/local/hphpi/legacy/bin/hhvm"
HHVM_VERSION="3.30.12"
MARIADB_PWD="password"

# 1. Install prerequisite packages
dnf install -y libevent-devel zlib-devel
dnf install -y php-common php-cli php-devel

# 2. Make sure hhvm 3.30.12 is installed
if ! which "$HHVM" >/dev/null 2>&1; then
  echo "Please install HHVM by the instruction first!"
  echo "If you have already installed, please link the hhvm executable to $HHVM."
fi

if [ "$($HHVM --version | head -n 1 | tr -d -c '0-9.')" != "$HHVM_VERSION" ]; then
  echo "oss-performance-mediawiki benchmark requires HHVM version $HHVM_VERSION."
fi

# 3. Install nginx
dnf install -y nginx
systemctl stop nginx

# 4. Install mariadb
dnf install -y mariadb-server
systemctl start mariadb
if ! [ -x "$(command -v mysql)" ]; then
  echo >&2 "Could not install mariadb!"
  exit 1
fi

mysql -u root --password="$MARIADB_PWD" -e ";"
mysql_success=$?
if [ $mysql_success -ne 0 ]; then
  mysql -uroot --password="" < "${TEMPLATES_DIR}/update_mariadb_pwd.sql"
fi

mysql -u root --password=$MARIADB_PWD < "${TEMPLATES_DIR}/grant_privileges.sql"

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

# 6. Install memcache
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

# 7. Installing OSS-performance

# shellcheck disable=SC2046
git clone https://github.com/hhvm/oss-performance.git
cd oss-performance || exit 1

# apply scale-out patch
# templates/oss-performance-mediawiki/0001-oss-performance-scalable-hhvm.diff
git apply --check "${TEMPLATES_DIR}/0001-oss-performance-scalable-hhvm.diff" \
    && git apply "${TEMPLATES_DIR}/0001-oss-performance-scalable-hhvm.diff"

# shellcheck disable=SC2046
curl -O https://getcomposer.org/installer
mv installer composer-setup.php

php composer-setup.php --2.2
yes | $HHVM composer.phar install

# 8. Basic tuning
echo 1 | sudo tee /proc/sys/net/ipv4/tcp_tw_reuse

# 9. MariaDB tuning
sudo cp "${TEMPLATES_DIR}/my.cnf" "/etc/my.cnf"
sudo systemctl restart mariadb

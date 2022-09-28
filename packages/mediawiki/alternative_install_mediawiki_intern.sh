#!/bin/bash

# set -e

TEMPLATES_DIR="$(dirname "$(readlink -f "$0")")"
HHVM="/usr/local/hphpi/legacy/bin/hhvm"
MARIADB_PWD="password"

# 1. Install prerequisite packages
dnf install -y libevent-devel zlib-devel
dnf install -y php-common php-cli php-devel

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
  mysql -u root --password="" < "${TEMPLATES_DIR}/update_mariadb_pwd.sql"
fi

mysql -u root --password=$MARIADB_PWD < "${TEMPLATES_DIR}/grant_privileges.sql"

# 5. Install Siege
if ! [ -x "$(command -v siege)" ]; then
  git clone https://github.com/JoeDog/siege.git
  cd siege || { exit 1; }
  git checkout tags/v4.0.3rc3
  ./utils/bootstrap
  automake --add-missing
  ./configure
  make -j8
  make uninstall
  make install
  cd ..
fi

# Copy siege.conf
mkdir -p "$HOME/.siege"
cp "${TEMPLATES_DIR}/siege.conf" "$HOME/.siege/siege.conf"
chmod 644 "$HOME/.siege/siege.conf"

# 6. Install memcache
if ! [ -d memcached-1.5.12 ]; then
  git clone https://github.com/pkuwangh/memcached-1.5.12.git
  # https://github.com/memcached/memcached/commit/8e5148ca5c32fafca948e43c2e966db28a50a5f4
  cd memcached-1.5.12 || { exit 1; }
  autoreconf -f -i
  ./configure --prefix=/usr/local/memcached
  make -j8
  make install
  cd ..
fi

# 7. Installing OSS-performance
git clone https://github.com/hhvm/oss-performance.git
cd oss-performance || { exit 1; }

curl -O https://getcomposer.org/installer
mv installer composer-setup.php

php composer-setup.php --2.2
yes | $HHVM composer.phar install

# 8. Basic tuning
echo 1 | sudo tee /proc/sys/net/ipv4/tcp_tw_reuse

# 9. MariaDB tuning
sudo cp "${TEMPLATES_DIR}/my.cnf" "/etc/my.cnf"
sudo systemctl restart mariadb

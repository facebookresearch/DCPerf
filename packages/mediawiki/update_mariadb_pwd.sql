/* Copyright (c) Meta Platforms, Inc. and affiliates.
   This source code is licensed under the MIT license found in the
   LICENSE file in the root directory of this source tree.
*/
UPDATE mysql.user SET Password=PASSWORD('password') WHERE User='root';
DELETE FROM mysql.user WHERE user='root' AND host NOT IN ('localhost', '127.0.0.1', '::1');
DELETE FROM mysql.user WHERE user='';
FLUSH PRIVILEGES;

UPDATE mysql.user SET Password=PASSWORD('password') WHERE User='root';
DELETE FROM mysql.user WHERE user='root' AND host NOT IN ('localhost', '127.0.0.1', '::1');
DELETE FROM mysql.user WHERE user='';
FLUSH PRIVILEGES;


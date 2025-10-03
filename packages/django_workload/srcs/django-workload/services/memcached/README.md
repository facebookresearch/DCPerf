# Memcached configuration

## Requirements
This directory sets up a memcached server with 5GB memory; you'll need a server
or VM with that amount of memory.

The server binds to *all network interfaces* so this should only be run in a
firewalled environment.

## Setup
This setup relies on the host system having memcached installed, we merely provide configuration as needed.
On Ubuntu 16.4 issue the following commands:

    apt-get install memcached

then the ./run-memcached script to run a new, separate server, as root (the
daemon switches to `memcache` user after starting)

    sudo ./run-memcached

## Logging
If needed, logging can be enabled for Memcached by adding the `-vv` switch in
the run-memcached script:
```
/usr/bin/memcached -vv -u $USER -m $MEMORY -l $LISTEN -p $PORT
```

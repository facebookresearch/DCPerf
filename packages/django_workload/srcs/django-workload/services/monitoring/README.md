# Monitoring configuration

## Requirements
This directory sets up a graphite server to track performance metrics.

Make sure you are running this in a firewalled environment.

We'll use docker to manage the installation, so follow the docker
installation instructions:

    https://docs.docker.com/engine/installation/linux/ubuntu/

Follow the instructions for the CE setup (Community Edition).

Next, install the hopsoft/graphite-statsd docker container, follow instructions
at:

    https://github.com/hopsoft/docker-graphite-statsd

Timeseries data can then be directed to UDP port 8125.

## Mandatory config
The default config of the hopsoft/graphite-statsd docker container will likely
cause your storage space to run out because of the amount of data being logged
into statsd by the Django Workload. In order to solve this, please perform the
steps below after starting the container. All commands should be run as root.

Obtain a shell in the container:
```
sudo docker exec -it graphite bash
cd opt/graphite/conf/
```

Add the following line to `blacklist.conf`:
```
^stats[^.]*\.benchmarkoutput\.
```

Edit the `carbon.conf` file to enable whitelisting. Search for the line
containing `USE_WHITELIST`, uncomment it and set it to True:
```
USE_WHITELIST = True
```

Edit the retention policy in `storage-schemas.conf`:
```
[default_1min_for_1day]
pattern = .*
retentions = 10s:2h,1min:2d,10min:14d
```

Exit the docker container and restart it for the configurations to take effect:
```
exit
docker stop graphite
docker start graphite
```

Should the disk space fill up again, you can simply delete graphite’s database:
```
docker exec -it graphite bash
rm –rf /opt/graphite/storage/whisper/*
```

# The Django server and uWSGI

This provides the views to be tested for the workload. This server
runs under uWSGI and connects to the other services
set up in a cluster. See the various subdirectories of the `services/` top-level
directory in this project.

## Requirements

The uWSGI server binds to *all network interfaces* so this should only be run in
a firewalled environment.

## Setup

On Ubuntu 16.04, you can run:

    apt-get install \
      build-essential \
      git \
      libmemcached-dev \
      python3-virtualenv \
      python3-dev \
      zlib1g-dev
    python3 -m virtualenv -p python3 venv
    source venv/bin/activate
    pip install -r requirements.txt

Next, copy the `cluster_settings_template.py` template to `cluster_settings.py`
and edit this to point to the various services running in the cluster:

    cp cluster_settings_template.py cluster_settings.py
    $EDITOR cluster_settings.py

Finally, run the `setup` django command to load the workload dataset into
Cassandra:

    DJANGO_SETTINGS_MODULE=cluster_settings django-admin setup

## Running

Start the service with

    uwsgi uwsgi.ini

and you can connect to port 8000 to access the server.

The above command should block. If it does not, check the django-uwsgi.log file
to see what went wrong.

## Debug

### uWSGI Logging
Logging for uWSGI is turned off by default for benchmarking purposes. In order
to turn it back on, comment out the `disable-logging` parameter in uwsgi.ini,
like this:
```
#disable-logging = True
```

### Django debugging
If you get HTTP response codes different than 200, change the DEBUG parameter
in cluster_settings.py:

    DEBUG = True

Then restart the uwsgi instance and go back to the page causing trouble. One of
the most common problems that cause 400 codes is not having the correct host in
the ALLOWED_HOSTS list in cluster_settings.py. If accessing the web server from
a different machine, add the hostname/IP address you are using to access the
server to ALLOWED_HOSTS.

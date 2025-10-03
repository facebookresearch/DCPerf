# Django workload by Instagram and Intel, v1.0 RC

This project aims to provide a Django workload based on a real-world
large-scale production workload that serves mobile clients.

## Setup for bare-metal

The project can be set up on a single machine, or on a cluster of machines
to spread the load and to make it easier to gauge the impact of the Django
workload on both Python and the hardware it runs on.

Documentation to set up each component of the cluster is provided for in
each subdirectory. You'll need to follow the [README.md](/README.md) file in each of the
following locations:

* 3 services
  * Cassandra - [services/cassandra/README.md](/services/cassandra/README.md)
  * Memcached - [services/memcached/README.md](/services/memcached/README.md)
  * Monitoring - [services/monitoring/README.md](/services/monitoring/README.md)

* Django and uWSGI - [django-workload/README.md](/django-workload/README.md)
* A load generator - [client/README.md](/client/README.md)

Once set up, access http://[uwsgi_host:uwsgi_port]/ to see an overview of
the offered endpoints, or use the load generator to produce a high request
load on the server.

## Setup for Docker containers

The workload can also be deployed using Docker containers. The instructions can
be found in [docker-scripts/README.md](/docker-scripts/README.md).

Please note that running the workload using Docker containers might deliver
less performance (transactions/second) than the bare-metal configuration and
there might be more run-to-run variation. In order to obtain the most accurate
performance comparison, please run the workload on bare-metal.

## Benchmarking configuration

The default benchmarking parameters used for Siege, Memcached and uWSGI are
suitable for driving high CPU utilization (>80%) in server environments:
```
uWSGI concurrent workers – 88
Memcached threads – 16
Siege Concurrency – 185
```

## Contributing

See the [CONTRIBUTING](/CONTRIBUTING.md) file for how to help out.

## License

Django Workload is [BSD-licensed](/LICENSE). We also provide an additional patent grant.

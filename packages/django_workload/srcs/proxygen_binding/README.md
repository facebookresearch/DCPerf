# DjangoBench V2: Asynchronous Server Stack \- Complete Guide

**An asynchronous Django server architecture using Proxygen \+ uWSGI \+ HAProxy to best match the IG Django in production**

## Table of Contents

- [DjangoBench V2: Asynchronous Server Stack - Complete Guide](#djangobench-v2-asynchronous-server-stack---complete-guide)
  - [Table of Contents](#table-of-contents)
  - [Motivation and Background](#motivation-and-background)
    - [Why We Developed This Stack](#why-we-developed-this-stack)
    - [Solution: Proxygen-based Async Stack](#solution-proxygen-based-async-stack)
  - [How to Build, Test and Run](#how-to-build-test-and-run)
    - [Install DjangoBench](#install-djangobench)
    - [System Requirements](#system-requirements)
    - [2.2 Run DjangoBench](#22-run-djangobench)
      - [Start Cassandra DB](#start-cassandra-db)
      - [Start benchmarking](#start-benchmarking)
      - [Using standalone configuration](#using-standalone-configuration)
      - [Parameters](#parameters)
  - [Evaluation](#evaluation)
    - [Scalability](#scalability)
    - [Microarch behavior](#microarch-behavior)
  - [Architecture and Design Deep Dive](#architecture-and-design-deep-dive)
    - [System Architecture](#system-architecture)
    - [Component Details](#component-details)
      - [Proxygen C++ HTTP Server](#proxygen-c-http-server)
      - [Python Binding (proxygen\_binding)](#python-binding-proxygen_binding)
      - [Django ASGI Adapter](#django-asgi-adapter)
      - [uWSGI Process Manager](#uwsgi-process-manager)
      - [HAProxy Load Balancer](#haproxy-load-balancer)
    - [Request Flow](#request-flow)
    - [Port Allocation Strategy](#port-allocation-strategy)
      - [High Tail Latency (P95, P99)](#high-tail-latency-p95-p99)
    - [uWSGI Integration Issues](#uwsgi-integration-issues)
      - [uWSGI Workers Not Spawning](#uwsgi-workers-not-spawning)
      - [Socket Error on uWSGI Startup](#socket-error-on-uwsgi-startup)
      - [Environment Variable Override](#environment-variable-override)
    - [HAProxy Buffer Overflow](#haproxy-buffer-overflow)
    - [Key Lessons Learned](#key-lessons-learned)
  - [Usage and Customization](#usage-and-customization)
    - [For DjangoBench Users (Benchpress)](#for-djangobench-users-benchpress)
      - [Quick Start](#quick-start)
      - [Available Jobs](#available-jobs)
    - [For Standalone Usage (Outside Benchpress)](#for-standalone-usage-outside-benchpress)
      - [Setup](#setup)
      - [Running the Server](#running-the-server)
      - [Test the Setup](#test-the-setup)
    - [Customization and Tuning](#customization-and-tuning)
      - [Worker Configuration](#worker-configuration)
      - [Port Configuration](#port-configuration)
      - [uWSGI Configuration](#uwsgi-configuration)
      - [HAProxy Configuration](#haproxy-configuration)
  - [References and Further Reading](#references-and-further-reading)
  - [Conclusion](#conclusion)
  - [Troubleshooting Executions](#troubleshooting-executions)
    - [Test with Simple Examples](#test-with-simple-examples)
      - [Basic Echo Server](#basic-echo-server)
      - [10.1.2 Django Integration Test](#1012-django-integration-test)
    - [Run Full Load-Balanced Server](#run-full-load-balanced-server)
      - [Direct Worker Mode (No uWSGI)](#direct-worker-mode-no-uwsgi)
      - [uWSGI Mode (Production)](#uwsgi-mode-production)
    - [Monitor and Test](#monitor-and-test)


## Motivation and Background

### Why We Developed This Stack

DjangoBench is Meta's web application benchmark that simulates Instagram-like
workloads. The original implementation used **synchronous uWSGI workers**, which
don't accurately represent Instagram's production architecture.

**Problem with Traditional uWSGI:**

- Synchronous blocking I/O, one request per worker at a time: poor scaling and
  low CPU utilization on CPUs with high core counts
- Not representative of Instagram's architecture: IG Django in production uses the asynchronous model

**Instagram's Production Architecture:** Instagram uses **asynchronous HTTP servers** in production to handle high concurrency:

- Achieving high throughput under concurrent load with async I/O and multi-threaded request processing
- IG Django's uses Proxygen as the webserver and integrates to the backend webapp with PyProxygen (Python binding).
  WSGI is still present but only in charge of worker process forking, reloading and memory leak cleanup (and doesn't actually serve traffic).

### Solution: Proxygen-based Async Stack

We developed a fully asynchronous server stack that mirrors Instagram's production architecture:

```
┌──────────────────────────────────────────────────────────────┐
│                    Production Architecture                   │
│          (Instagram Django + Proxygen + Pickwick)            │
└──────────────────────────────────────────────────────────────┘
                            │
                            │ Mirrored in DjangoBench V2
                            ▼
┌────────────────────────────────────────────────────────────────────────────────────────┐
│  Client Requests → HAProxy (Load Balancer) → uWSGI Workers 1   ...    uWSGI Worker N   │
│                                                    ↓                          ↓        │
│                                            Proxygen (Async HTTP)    Proxygen (Async)   │
│                                                    ↓                          ↓        │
│                                            Django ASGI App          Django ASGI App    │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

**Key Components:**

- **Proxygen**: Meta's C++ async HTTP server (used at Instagram)
- **uWSGI**: Process management, memory cleanup, monitoring
- **HAProxy**: Load balancing with health checks
- **Django ASGI**: Async application layer

**Benefits:**

- ✅ **Realistic benchmark**: Software stack matches Instagram's production architecture
- ✅ **Better CPU utilization**: Async HTTP requests handling and balanced load across all workers
- ✅ **Higher Scalability**: Relative speedup more linear as core count

---

## How to Build, Test and Run

### Install DjangoBench

The command for building and installing DjangoBench remains the same:

```shell
./benchpress_cli.py install django_workload_default
```

### System Requirements

* If using the distributed setup, We recommend placing the DB server machine and
  the benchmarking machine within the same network and maintain the ping latency
  between them to be in the range of 0.1 and 0.15ms.
* **Port availability**: the following ports need to be available on the machines:
  * Cassandra DB node: port 9042
  * Clientserver node:
    * Main HTTP server port: 8000
    * Load balancer stats port: 8001
    * Memcached port: 11811
    * Server worker ports: The range of \[16667, 16667 \+ `server_workers`)
      (`server_workers` is equal to the number of CPU logical cores).
    * Load balancer stats port and server worker starting port are adjustable,
      but the continuous range of server worker ports must be available.

### 2.2 Run DjangoBench

The way of running DjangoBench also remains the same: ideally we recommend
having one separate server for the Cassandra DB and another for the client and
server where the benchmark is executed and measured. There is an option for
running everything on a single machine if you are unable to run distributed
setup.

#### Start Cassandra DB

On the Cassandra DB server machine:

```
./benchpress_cli.py run django_workload_default -r db
```

This should run indefinitely. You will see a lot of java processes running, and
you can check if Cassandra has started up successfully by running
`lsof -i -P -n | grep 9042`. Cassandra will also output log at benchmarks/django\_workload/cassandra.log.

If you would like Cassandra DB to bind a custom address, please use the following command:

```
./benchpress_cli.py run django_workload_default -r db -i '{"bind_ip": "<ip_addr>"}'
```

This is useful when the output of `hostname -i` does not return a reachable IP
address or is not the address you would like to use. Please see more details in
[Troubleshooting](https://github.com/facebookresearch/DCPerf/tree/main/packages/django_workload#troubleshooting).

#### Start benchmarking

On the django benchmarking machine (where the django server and client are run):

```
./benchpress_cli.py run django_workload_default -r clientserver -i '{"db_addr": "<db-server-ip>"}'
```

Note that `<db-server-ip>` has to be an IP address, hostname will not work.

If running on ARM platform, please use the job `django_workload_arm`:

```
./benchpress_cli.py run django_workload_arm -r clientserver -i '{"db_addr": "<db-server-ip>"}'
```

#### Using standalone configuration

To run the server, client and database on the same benchmarking machine:

```
./benchpress_cli.py run django_workload_default -r standalone
```

If running on ARM platform, please use the job django\_workload\_arm:

```
./benchpress_cli.py run django_workload_arm -r standalone
```

#### Parameters

We provide the following parameters you can customize for DjangoBench workload:

For `django_workload_default` and `django_workload_arm` jobs:

* Role `clientserver`:
  * `db_addr` \- **required**, IP address of the Cassandra server
  * `duration` \- Duration of each iteration of test, default `5M` (5 minutes)
  * `iterations` \- Number of iterations to run, default 7
  * `reps`: Number of requests (per client worker) that the load generator will send in each iteration.
    This will override `duration` and is useful to workaround the hanging problem of Siege (the load generator).
    Note the total number of requests that Siege will send will be `reps * client_workers`, where
    `client_workers = 1.2 * NPROC`.
  * `interpreter` \- Which python interpreter to use: choose between `cpython` or `cinder`.
    Defaults to `cpython`.
  * `use_async` \- If this is set to 1, DjangoBench will use this new asynchronous server stack;
    set to 0 means using the traditional stack. Defaults to 1.
  * `base_port` \- Starting port that the HTTP server workers will listen to.
    The range of `[base_port, base_port + nproc)` must be available.
  * `stats_port` \- Load balancer stats port, defaults 8001
* Role `standalone`:
  * Same as role `clientserver` except not having `db_addr`

For `django_workload_custom`:

* Role `clientserver`, there are these extra parameters:
  * `server_workers` \- number of server workers, required.
  * `client_workers` \- number of client workers, required
  * `ib_min` \- ICacheBuster minimum iterations in each request (default 100000\)
  * `ib_max` \- ICacheBuster maximum iterations in each request (default 200000\)

---

## Evaluation

### Scalability

DjangoBench showed much better scalability and CPU utilization on CPUs with high
core counts after adopting Proxygen and the asynchronous server model.

In DjangoBench v1, CPU utilization declined significantly (to less than 80\%)
when run on CPUs of more than 300 logical cores. After adopting the asynchronous
server model, the CPU utilization and scalability improved significantly on
those CPUs with ultra high core counts.  While the absolute RPS numbers
decreased because each request involved more work (measured by MIPS/QPS), the
relative perf correlation was much better especially on the CPU with 384 logical
cores in our datacenter. This shows the advantage of adopting the asynchronous
server model, where each web server worker can handle multiple requests
concurrently and therefore there'll be less stranded CPU cycles.

### Microarch behavior

This work of Proxygen integration and asynchronous server model adoption
increased L1-ICache MPKI by 6\~8 on AMD Zen4. After using Cinder with JIT, the
L1-MPKI increased further, very close to the original version even without
ICacheBuster.

## Architecture and Design Deep Dive

### System Architecture

```
┌─────────────────────────────────────────────────────────────┐
│                      Client Applications                    │
│             (Siege, curl, benchmark clients)                │
└──────────────────┬──────────────────────────────────────────┘
                   │ HTTP Requests
                   ▼
┌──────────────────────────────────────────────────────────────┐
│                   HAProxy Load Balancer                      │
│  - Port: 8000 (frontend)                                     │
│  - Algorithm: leastconn (least connections)                  │
│  - Health checks: GET / every 2s                             │
│  - Stats dashboard: http://127.0.0.1:9000/stats              │
└─────────────┬────────────────┬────────────────┬──────────────┘
              │                │                │
              ▼                ▼                ▼
┌─────────────────────┐  ┌─────────────────────┐  ┌────────────
│   uWSGI Worker 1    │  │   uWSGI Worker 2    │  │  Worker N
│   PID: 12346        │  │   PID: 12347        │  │  PID: ...
│ ┌─────────────────┐ │  │ ┌─────────────────┐ │  │ ┌─────────
│ │ Proxygen Server │ │  │ │ Proxygen Server │ │  │ │ Proxygen
│ │  0.0.0.0:8001   │ │  │ │  0.0.0.0:8002   │ │  │ │  0.0.0.0
│ │                 │ │  │ │                 │ │  │ │
│ │  14 Threads     │ │  │ │  14 Threads     │ │  │ │  14 Thre
│ │  ┌───┐ ┌───┐    │ │  │ │  ┌───┐ ┌───┐    │ │  │ │  ┌───┐
│ │  │ T │ │ T │    │ │  │ │  │ T │ │ T │    │ │  │ │  │ T │
│ │  │ 1 │ │ 2 │... │ │  │ │  │ 1 │ │ 2 │... │ │  │ │  │ 1 │
│ │  └─┬─┘ └─┬─┘    │ │  │ │  └─┬─┘ └─┬─┘    │ │  │ │  └─┬─┘
│ └────┼─────┼──────┘ │  │ └────┼─────┼──────┘ │  │ └────┼────
│      │     │        │  │      │     │        │  │      │
│      ▼     ▼        │  │      ▼     ▼        │  │      ▼
│ ┌─────────────────┐ │  │ ┌─────────────────┐ │  │ ┌─────────
│ │ ASGI Adapter    │ │  │ │ ASGI Adapter    │ │  │ │ ASGI Ada
│ │ (asyncio loop)  │ │  │ │ (asyncio loop)  │ │  │ │ (asyncio
│ └────────┬────────┘ │  │ └────────┬────────┘ │  │ └────┬────
│          │          │  │          │          │  │       │
│          ▼          │  │          ▼          │  │       ▼
│ ┌─────────────────┐ │  │ ┌─────────────────┐ │  │ ┌─────────
│ │  Django ASGI    │ │  │ │  Django ASGI    │ │  │ │  Django
│ │  Application    │ │  │ │  Application    │ │  │ │  Applica
│ │                 │ │  │ │                 │ │  │ │
│ │  - Views        │ │  │ │  - Views        │ │  │ │  - Views
│ │  - Models       │ │  │ │  - Models       │ │  │ │  - Model
│ │  - Middleware   │ │  │ │  - Middleware   │ │  │ │  - Middl
│ └────────┬────────┘ │  │ └────────┬────────┘ │  │ └────┬────
│          │          │  │          │          │  │       │
│          ▼          │  │          ▼          │  │       ▼
└──┬────────────────┬─┘  └──┬────────────────┬─┘  └──┬─────────
   │                │       │                │       │
   ▼                ▼       ▼                ▼       ▼
┌────────────────────────┐  ┌────────────────────────────────┐
│       Cassandra        │  │            Memcached           │
│        Database        │  │             (Cache)            │
└────────────────────────┘  └────────────────────────────────┘
```

### Component Details

#### Proxygen C++ HTTP Server

**What is Proxygen?**

- Meta's open-source C++ HTTP library
- Used in production at Instagram, Facebook, WhatsApp
- Async I/O with multi-threading
- High performance, low latency

**Key Features:**

- **Event-driven architecture**: Non-blocking I/O
- **Thread pool**: N threads handle concurrent requests
- **HTTP/1.1 and HTTP/2** support
- **Connection pooling**: Keep-alive connections
- **Memory efficient**: Zero-copy where possible

**In DjangoBench:**

- Each uWSGI worker runs one Proxygen server
- Proxygen binds to unique port (8001, 8002, ...)
- Each Proxygen server has multiple threads (configurable)
- Threads handle HTTP requests asynchronously

#### Python Binding (proxygen\_binding)

**Purpose:** Bridge C++ Proxygen to Python Django application

**Files:**

- `PythonRequestHandler.cpp/h`: C++ request handler
- `proxygen_binding.cpp`: pybind11 binding code
- `django_asgi_adapter.py`: ASGI protocol adapter
- `example_server.py`: Basic usage example
- `django_server.py`: Django integration example
- `event_loop_manager.py`: Event loop management

**How It Works:**

```py
# C++ creates RequestData from HTTP request
RequestData {
    method: "GET",
    url: "http://localhost:8000/timeline",
    path: "/timeline",
    query_string: "",
    headers: {"Host": "localhost:8000", ...},
    body: "",
    http_version: "HTTP/1.1"
}

# Python callback receives RequestData
def handler(request: RequestData) -> ResponseData:
    # Convert to ASGI scope
    scope = {
        'type': 'http',
        'method': request.method,
        'path': request.path,
        'headers': request.headers,
        ...
    }

    # Call Django ASGI app
    response = await asgi_app(scope, receive, send)

    # Return ResponseData
    return ResponseData(
        status_code=200,
        status_message="OK",
        headers={"Content-Type": "text/html"},
        body="<html>...</html>"
    )
```

**Thread Safety:**

- GIL (Global Interpreter Lock) properly managed
- GIL released during C++ operations
- GIL acquired for Python callbacks
- Each thread has isolated asyncio event loop

#### Django ASGI Adapter

**File:** `django_asgi_adapter.py`

**Purpose:** Convert between Proxygen's request format and Django's ASGI protocol

**ASGI Protocol Overview:**

ASGI (Asynchronous Server Gateway Interface) is Django's async protocol:

```py
async def application(scope, receive, send):
    """
    scope: dict with request metadata
    receive: async callable to get request body
    send: async callable to send response
    """
    # Django processes request
    # Calls send() to return response
```

**Our Adapter:**

```py
class ASGIRequestHandler:
    def __init__(self, asgi_app):
        self.asgi_app = asgi_app  # Django's get_asgi_application()

    def __call__(self, request: RequestData) -> ResponseData:
        # 1. Convert RequestData → ASGI scope
        scope = self._build_scope(request)

        # 2. Create receive/send callables
        async def receive():
            return {'type': 'http.request', 'body': request.body}

        responses = []
        async def send(message):
            responses.append(message)

        # 3. Call Django ASGI app
        loop = asyncio.new_event_loop()
        loop.run_until_complete(self.asgi_app(scope, receive, send))

        # 4. Collect response and convert to ResponseData
        return self._build_response(responses)
```

**Event Loop Management:**

Each Proxygen thread gets its own asyncio event loop:

```py
# In event_loop_manager.py
_thread_local = threading.local()

def get_event_loop():
    if not hasattr(_thread_local, 'loop'):
        _thread_local.loop = asyncio.new_event_loop()
        asyncio.set_event_loop(_thread_local.loop)
    return _thread_local.loop
```

**Why Separate Event Loops?**

- asyncio is NOT thread-safe
- Each thread needs isolated event loop
- Prevents race conditions
- Better performance (no locks)

#### uWSGI Process Manager

**Role:** Process management only (not HTTP serving)

**Responsibilities:**

- Fork N worker processes
- Monitor worker health
- Restart dead workers
- Memory leak detection (reload-on-rss)
- Timeout enforcement (harakiri)
- Graceful reload (SIGHUP)

**Configuration:** `uwsgi_loadbalanced.ini`

```
[uwsgi]
master = true
processes = %(workers)  # Overridden by --set workers=N
lazy-apps = true        # Load app after fork

# Memory management
reload-on-rss = 2048    # Reload at 2GB RAM
harakiri = 60           # Kill after 60s timeout

# NO HTTP BINDING - Proxygen handles HTTP
socket = /tmp/uwsgi-loadbalanced.sock
```

**Worker Lifecycle:**

```py
# In proxygen_wsgi.py

# 1. uWSGI imports module
import proxygen_wsgi

# 2. uWSGI forks worker
# postfork hook triggered

# 3. Start Proxygen in worker
@postfork
def start_proxygen_server():
    # Calculate port from worker ID
    worker_id = uwsgi.worker_id()  # 1, 2, 3, ...
    port = base_port + worker_id - 1  # 8001, 8002, 8003, ...

    # Create Django handler
    handler = create_django_handler()

    # Start Proxygen server
    server = ProxygenServer(
        ip="0.0.0.0",
        port=port,
        threads=14,
        callback=handler
    )
    server.start()

# 4. Register cleanup
@atexit.register
def cleanup():
    server.stop()
```

#### HAProxy Load Balancer

**Role:** Distribute traffic across workers

**Algorithm:** `leastconn` (least connections)

- Routes requests to worker with fewest active connections
- Optimal for async servers (workers have varying loads)
- Better than round-robin for uneven request costs

**Configuration:** Auto-generated by `start_loadbalanced_server.py`

```
frontend django_frontend
    bind *:8000
    maxconn 50000
    default_backend django_workers

backend django_workers
    balance leastconn

    # Health checks
    option httpchk GET /
    http-check expect status 200

    # Workers
    server worker1 127.0.0.1:8001 check weight 1 maxconn 10000
    server worker2 127.0.0.1:8002 check weight 1 maxconn 10000
    ...

listen stats
    bind *:9000
    stats enable
    stats uri /stats
```

**Health Checks:**

- HAProxy pings each worker every 2 seconds
- Expects HTTP 200 response
- Marks worker DOWN after 3 failures
- Auto-routes traffic to healthy workers

### Request Flow

**Complete Request Journey:**

```
1. Client sends HTTP request
   ↓
2. HAProxy receives on port 8000
   - Checks worker health status
   - Selects worker with least connections (leastconn)
   ↓
3. HAProxy forwards to Worker 2 (port 8002)
   ↓
4. Proxygen receives in Worker 2
   - One of 14 threads picks up request
   - Parses HTTP headers and body
   ↓
5. Proxygen creates PythonRequestHandler (C++)
   - Collects request data into RequestData struct
   ↓
6. pybind11 bridge: C++ → Python
   - GIL acquired
   - RequestData marshaled to Python
   - Python callback invoked
   ↓
7. Django ASGI Adapter processes
   - Converts RequestData → ASGI scope dict
   - Creates receive() and send() callables
   - Gets asyncio event loop for this thread
   ↓
8. Django ASGI application executes
   - URL routing: /timeline → views.timeline_view()
   - Middleware stack processes request
   - View function executes
   - Database query: SELECT * FROM feed_entry_model
   - Cache query: Memcached GET user:123:feed
   - Template rendering
   ↓
9. Django returns via ASGI send()
   - send({'type': 'http.response.start', ...})
   - send({'type': 'http.response.body', 'body': ...})
   ↓
10. ASGI Adapter collects response
    - Assembles ResponseData from send() calls
    ↓
11. pybind11 bridge: Python → C++
    - ResponseData marshaled back to C++
    - GIL released
    ↓
12. Proxygen sends HTTP response
    - Headers sent
    - Body sent (chunked or content-length)
    - Connection closed or kept alive
    ↓
13. HAProxy forwards response to client
    ↓
14. Client receives response
```

**Timing Example:**

```
Request to /timeline endpoint:
- HAProxy routing: 0.1ms
- Proxygen parsing: 0.2ms
- Python marshaling: 0.1ms
- Django routing: 0.3ms
- Database query: 5ms
- Cache query: 1ms
- Template render: 3ms
- Response marshaling: 0.1ms
- Proxygen send: 0.2ms
Total: ~10ms
```

### Port Allocation Strategy

**Problem:** Multiple workers need unique ports

**Solution:** Initially we enabled the socket option `SO_REUSEPORT` to allow
  reusing a single port by multiple server workers, but it will have the problem
  of different workers having very varied loads. Therefore, we used HAProxy load
  balancer to address this issue.:

```shell
# Install HAProxy
sudo dnf install -y haproxy

# Create HAProxy config
python start_loadbalanced_server.py --workers 8
```

**Results:**

```
Worker 1: 75% CPU (balanced)
Worker 2: 76% CPU (balanced)
...
Worker 7: 74% CPU (balanced)
Worker 8: 75% CPU (balanced)

Variance: < 2% (improved from 20%)
```

#### High Tail Latency (P95, P99)

**Symptom:**

```
P50: 0.03s  (acceptable)
P95: 0.14s  (4.6x worse than uWSGI)
P99: 0.21s  (3.5x worse than uWSGI)
```

**Root Cause:**

- Hot workers have request queue buildup
- Cold workers sit idle
- No distribution of work

**Solution:** After adding HAProxy with leastconn algorithm:

```
P50: 0.02s  (baseline)
P95: 0.03s  (4.6x improvement!)
P99: 0.06s  (3.5x improvement!)
```

### uWSGI Integration Issues

#### uWSGI Workers Not Spawning

**Symptom:**

```
[uWSGI] *** Operational MODE: no-workers ***
```

**Root Cause:**

- Used `workers = N` instead of `processes = N`
- uWSGI configuration error

**Solution:**

```
# Wrong
workers = 8

# Correct
processes = 8
```

#### Socket Error on uWSGI Startup

**Symptom:**

```
The -s/--socket option is missing and stdin is not a socket
```

**Root Cause:**

- uWSGI requires socket configuration to start
- Even if not used for HTTP

**Solution:** Added socket to `uwsgi_loadbalanced.ini`:

```
socket = /tmp/uwsgi-loadbalanced.sock
chmod-socket = 666
```

#### Environment Variable Override

**Symptom:**

```
Workers binding to wrong ports
All workers try to bind to port 8001
```

**Root Cause:**

- `env =`  directives in uWSGI config override Python script's environment
- Port calculation broken

**Solution:** Removed `env =` directives from config, let Python script set environment:

```
# Don't do this - it overrides Python script's environment
# env = PROXYGEN_BASE_PORT=8001
# env = PROXYGEN_THREADS=14

# Let Python script set environment variables
```

### HAProxy Buffer Overflow

**Symptom:**

```
[HAProxy] logs stop appearing
Script appears to hang
```

**Root Cause:**

- Python's `subprocess.Popen()` has limited buffer
- HAProxy produces lots of output
- Buffer fills up, HAProxy blocks

**Solution:** Real-time log streaming with threads:

```py
def _stream_logs(self, process, prefix, log_file=None):
    """Stream logs in real-time to prevent buffer overflow"""
    for line in iter(process.stdout.readline, ""):
        if self.stop_logging.is_set():
            break
        if line:
            print(f"{prefix} {line.rstrip()}", flush=True)
            if log_file:
                log_file.write(f"{prefix} {line}")
                log_file.flush()
```

### Key Lessons Learned

1. **Event loops are per-thread**: Never share asyncio event loops between threads
2. **GIL management is critical**: Always acquire GIL for Python, release for C++
3. **Thread pools matter**: Default 8 threads not enough for async DB queries
4. **Load balancing is essential**: Raw workers don't distribute load evenly
5. **Buffer management**: Always stream subprocess output in real-time
6. **Port conflicts**: Check port availability before starting servers
7. **Environment variables**: Be careful with uWSGI `env =` directives

---

## Usage and Customization

### For DjangoBench Users (Benchpress)

**NOTE**: for regular DjangoBench usage, we've provided instructions in the
[How to Build, Test and Run](#how-to-build-test-and-run) section and also in the
benchmark's main [README](../../README.md).

#### Quick Start

**Run with async mode (default):**

```shell
cd <DCPerf-folder>

# Run default job with async stack
./benchpress_cli.py run django_workload_default

# This automatically uses:
# - uWSGI process management
# - Proxygen async HTTP
# - HAProxy load balancing
# - $(nproc) workers
```

**Customize workers:**

```shell
# Override number of workers
./benchpress_cli.py run django_workload_custom -r clientserver -i '{"server_workers": 16, "client_workers": 32}'

# Disable async mode (fall back to traditional uWSGI)
./benchpress_cli.py run django_workload_default -r clientserver -i '{"use_async": 0}'
```

#### Available Jobs

| Job Name | Description | Use Case |
| :---- | :---- | :---- |
| `django_workload_default` | Standard run (5M duration) | Full benchmark |
| `django_workload_arm` | ARM-optimized params | ARM testing |
| `django_workload_mini` | Quick test (1000 reps) | Emulation support |
| `django_workload_custom` | More customizable params | Tuning experiments |

**All jobs default to async mode (`use_async=1`)**

### For Standalone Usage (Outside Benchpress)

**NOTE**: This section is not necessary for running DjangoBench. Instead, it is more
of a "development guide" for people who would like to tweak, customize and debug
DjangoBench. Also see [Troubleshooting Executions](#troubleshooting-executions) besides
the instructions in this section if something is broken and you would like to debug.

#### Setup

**Prepare Environment:**

```shell
# Under the DCPerf repo folder

# Activate virtual envrionment
source benchmarks/django_workload/django-workload/django-workload/venv_cpython/bin/activate

# Add library paths
export LD_LIBRARY_PATH="$(pwd)/benchmarks/django_workload/django-workload/django-workload/Python-3.10.2:$(pwd)/benchmarks/django_workload/django-workload/django-workload"

# Set Django settings
export DJANGO_SETTINGS_MODULE=cluster_settings
```

**Start Cassandra and Memcached:**

```shell
# Start Cassandra
./benchmarks/django_workload/bin/run.sh -r db -b 127.0.0.1 &

# Start Memcached
./services/memcached/run-memcached &

# Populate database
cd benchmarks/django_workload/django-workload/django-workload
DJANGO_SETTINGS_MODULE=cluster_settings django-admin flush
DJANGO_SETTINGS_MODULE=cluster_settings django-admin setup
```

#### Running the Server

**Option 1: Direct Workers (Python-managed):**

```shell
# Assuming you are under the root of DCPerf repo folder

cd benchmarks/django_workload/django-workload/django-workload
python start_loadbalanced_server.py \
    --workers 8 \
    --base-port 8001 \
    --lb-port 8000 \
    --stats-port 9000 \
    --threads-per-worker 14 \
    --log-dir logs
```

**Option 2: uWSGI-managed Workers:**

```shell
cd benchmarks/django_workload/django-workload/django-workload
DJANGO_SETTINGS_MODULE=cluster_settings \
python start_loadbalanced_server.py \
    --use-uwsgi \
    --workers $(nproc) \
    --stats-port 9000 \
    --log-dir uwsgi_logs
```

#### Test the Setup

```shell
# Basic connectivity test
curl http://127.0.0.1:8000/

# Test endpoints
curl http://127.0.0.1:8000/timeline
curl http://127.0.0.1:8000/feed_timeline
curl http://127.0.0.1:8000/bundle_tray
curl http://127.0.0.1:8000/inbox

# Check stats
open http://127.0.0.1:9000/stats
```

### Customization and Tuning

#### Worker Configuration

**Adjust worker count based on CPU:**

```shell
# CPU-bound workload
python start_loadbalanced_server.py --workers 16 --threads-per-worker 8

# I/O-bound workload
python start_loadbalanced_server.py --workers 4 --threads-per-worker 32

# Balanced (default)
python start_loadbalanced_server.py --workers 8 --threads-per-worker 14
```

#### Port Configuration

```shell
# Custom ports
python start_loadbalanced_server.py \
    --base-port 9001 \      # Workers start at 9001
    --lb-port 9000 \        # Frontend on 9000
    --stats-port 9090       # Stats on 9090
```

#### uWSGI Configuration

Edit `uwsgi_loadbalanced.ini` for uWSGI-specific settings:

```
[uwsgi]
# Process settings
master = true
processes = %(workers)
lazy-apps = true

# Memory management
reload-on-rss = 2048        # Reload worker at 2GB
harakiri = 60               # Kill after 60s timeout
max-requests = 10000        # Reload after 10K requests

# Environment (set by Python script)
# Don't use env= directives here
```

#### HAProxy Configuration

Auto-generated config can be customized by editing the generated file:

```shell
# After first run, config is saved
ls -la haproxy_generated.cfg

# Edit and restart
vim haproxy_generated.cfg
python start_loadbalanced_server.py --haproxy-config haproxy_generated.cfg
```


## References and Further Reading

- [Proxygen GitHub](https://github.com/facebook/proxygen)
- [Django ASGI](https://docs.djangoproject.com/en/stable/howto/deployment/asgi/)
- [HAProxy Documentation](http://www.haproxy.org/docs.html)
- [uWSGI Documentation](https://uwsgi-docs.readthedocs.io/)
- [pybind11 Documentation](https://pybind11.readthedocs.io/)


## Conclusion

This async server stack brings DjangoBench in line with Instagram's production architecture, delivering
improved scalability and increased CPU front-end I-Cache presssure.

## Troubleshooting Executions

### Test with Simple Examples

#### Basic Echo Server

Test the Proxygen binding with a simple echo server:

```shell
# Activate virtual envrionment
source benchmarks/django_workload/django-workload/django-workload/venv_cpython/bin/activate

# Add library paths
export LD_LIBRARY_PATH="$(pwd)/benchmarks/django_workload/django-workload/django-workload/Python-3.10.2:$(pwd)/benchmarks/django_workload/django-workload/django-workload"

# Start example server
cd benchmarks/django_workload/proxygen_binding
python3 example_server.py
```

**In another terminal:**

```shell
# Test the server
curl http://127.0.0.1:8000/abc
# Expected:
# Proxygen Python Binding - Echo Server
# ==================================================
#
# Method: GET
# Path: /abc
# URL: /abc
# Query String:
# HTTP Version: 1.1
#
# Headers:
#   Accept: */*
#   Host: localhost:8000
#   User-Agent: curl/7.76.1

# Test POST with data
curl -X POST -d "test data" http://127.0.0.1:8000/echo
# Expected: Echo of posted data
```

**What This Tests:**

- ✓ Proxygen binding works
- ✓ Request/response marshaling
- ✓ Multi-threaded handling

#### 10.1.2 Django Integration Test

Test Django ASGI integration:

```shell
# Activate virtual envrionment
source benchmarks/django_workload/django-workload/django-workload/venv_cpython/bin/activate

# Add library paths
export LD_LIBRARY_PATH="$(pwd)/benchmarks/django_workload/django-workload/django-workload/Python-3.10.2:$(pwd)/benchmarks/django_workload/django-workload/django-workload"

# Set Django settings
export DJANGO_SETTINGS_MODULE=cluster_settings

# Start Django server with Proxygen
cd benchmarks/django_workload/proxygen_binding
python3 django_server.py
```

**Test endpoints:**

```shell
# Test homepage
curl http://127.0.0.1:8000/

# Test timeline (will error out if DB not set up)
curl http://127.0.0.1:8000/timeline
```

**What This Tests:**

- ✓ Django ASGI adapter works
- ✓ Async request handling
- ✓ Database/cache integration

### Run Full Load-Balanced Server

Please first follow the setup steps in [For Standalone Usage (Outside Benchpress)](#for-standalone-usage-outside-benchpress)
section to prepare the environment.

#### Direct Worker Mode (No uWSGI)

Run multiple workers managed by Python script:

```shell
cd benchmarks/django_workload/django-workload/django-workload

# Start 8 workers with HAProxy
python start_loadbalanced_server.py \
    --workers 8 \
    --stats-port 9000 \
    --log-dir load_balancer_logs
```

**Output:**

```
[15:23:45] [INFO] Starting 8 Proxygen workers...
[15:23:46] [INFO] Worker 1 started (PID: 12345)
[15:23:46] [INFO] Worker 2 started (PID: 12346)
...
[15:23:47] [INFO] All workers are ready!
[15:23:48] [INFO] HAProxy started (PID: 12353)
============================================================
Load-balanced Django server is running!
  Access via: http://127.0.0.1:8000
  Workers: 8 (ports 8001-8008)
  Stats UI: http://127.0.0.1:9000/stats

Press Ctrl+C to stop all processes
============================================================
```

#### uWSGI Mode (Production)

Run with uWSGI managing Proxygen workers:

```shell
cd benchmarks/django_workload/django-workload/django-workload

# Start with uWSGI + HAProxy
DJANGO_SETTINGS_MODULE=cluster_settings \
python start_loadbalanced_server.py \
    --use-uwsgi \
    --workers $(nproc) \
    --stats-port 9000 \
    --log-dir uwsgi_logs
```

**This is the production-ready mode with:**

- ✅ uWSGI process management (harakiri, memory limits)
- ✅ Proxygen async HTTP in each worker
- ✅ HAProxy load balancing
- ✅ Health checks and auto-recovery

### Monitor and Test

**Access HAProxy Stats Dashboard:**

```
http://127.0.0.1:9000/stats
```

**Run Load Tests:**

```shell
# Basic test
curl http://127.0.0.1:8000/timeline

# Concurrent test
siege -b -r 1000 -c 112 http://127.0.0.1:8000/timeline

# Expected results:
# - All requests succeed
# - Balanced load across workers
# - Low tail latency
```

**Monitor CPU Usage:**

```shell
# Watch worker processes
watch -n 1 'ps aux | grep proxygen_wsgi | grep -v grep'

# Should show balanced CPU across all workers
```

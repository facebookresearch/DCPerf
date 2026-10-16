# Adsim
Adsim is a benchmark designed to represent machine learning (ML) inference workloads in a client-server architecture. It operates in two distinct roles: **client** and **server**.
- **Server:** Listens on a specified port and awaits incoming requests.
- **Client:** Sends inference requests to the server.

## Installation
To install Adsim, execute the following command:
```bash
./benchpress -b ai install adsim_inference_a
```

## Run Adsim
### Job - `adsim_inference_a`

The `adsim_inference_a` job simulates the inference workload for Model A. Its server pipeline runs the `TensorDeser`, `TorchScriptOps`, `DenseFP32`, `Rebatch`, `GpuSpinWait`, decoding `Decompress`, and `FakeIO` kernels. The `TorchScriptOps` kernels run ATen operators through libtorch, so the server is `adsim_server_torch` (`--server-binary adsim_server_torch`), which the install builds next to `adsim_server`. To configure the client-server benchmark, follow these steps:
1. Start the server
~~~bash
# Setup the server
./benchpress -b ai run adsim_inference_a -r server
~~~
2. Start the client
~~~bash
# Setup the client
./benchpress -b ai run adsim_inference_a -r client --role_input='{"server_ip":"<server-ip>"}'
~~~

The client searches for the QPS whose p99 latency is closest to 150 ms, with request size multiplier 1. Each search point runs for 120 seconds, and a search can take more than 30 minutes, so the client timeout is 3600 seconds.

The server has these options:
- The server runs on 48 logical CPUs (`num_cpus=48`). It takes whole SMT cores in ascending CPU order from the CPUs that benchpress may use, so `taskset -c <cpus> ./benchpress ...` chooses where the server runs. The server fails to start if fewer CPUs are available. Set `num_cpus=0` to use all of them.
- The server's fbthrift sends replies with `EventBase::runInEventBaseThread` instead of the IO thread's reply queue (`reply_via_event_base=1`, which passes `--thrift_reply_via_event_base` to the server). `build-deps.sh` adds the flag to fbthrift with `patches/fbthrift-reply-via-event-base.patch`. Set `reply_via_event_base=0` to use the reply queue, fbthrift's default.

## Reporting and Measurement
After the adsim benchmark finishing, benchpress will report the results in
JSON format like the following:

```json
{
  "benchmark_args": [
    "client",
    "--server ::1",
    "--port 10086",
    "--workers 20",
    "--runtime 120",
    "--criteria P99",
    "--latency 150",
    "--timeout 3600",
    "--mode ai",
    "--ai-request-size-multiplier 1"
  ],
  "benchmark_desc": "Benchmark for Model Inference Workload, with ATen ops from libtorch (P99 150 ms, server on 48 CPUs)",
  "benchmark_hooks": [
    "perf: None",
    "fb_stop_dynologd: None",
    "fb_chef_off_turbo_on: None"
  ],
  "benchmark_name": "adsim_inference_a",
  "machines": [
    {
      "cpu_architecture": "x86_64",
      "cpu_model": "<CPU-name>",
      "hostname": "<host-name>",
      "kernel_version": "6.4.3-0_xxxxx",
      "mem_total_kib": "2377089692 KiB",
      "num_cpus_usable": 384,
      "num_logical_cpus": "384",
      "os_distro": "centos",
      "os_release_name": "CentOS Stream 9",
      "threads_per_core": "2"
    }
  ],
  "metadata": {
    "L1d cache": "6 MiB (192 instances)",
    "L1i cache": "6 MiB (192 instances)",
    "L2 cache": "192 MiB (192 instances)",
    "L3 cache": "768 MiB (24 instances)"
  },
  "metrics": {
    "final_achieved_qps": 2533.61,
    "final_latency_msec": 43.868,
    "final_requested_qps": 2533.0,
    "success": true,
    "target_latency_msec": 100.0,
    "target_latency_percentile": "P99"
  },
  "run_id": "ad65e3d5",
  "timestamp": 1755091757,
}
```
The `metrics` section contains the key performance indicators for the Adsim benchmark:
- **`final_achieved_qps`**: The final achieved queries per second (QPS) measured on the server.
- **`final_requested_qps`**: The final requested QPS as issued by the client.
- **`average_latency_msec`**: The average latency per request, measured in milliseconds.

# Syscall Benchmarks

Benchpress supports several syscall benchmarks. By default, it benchmarks all of them.

Install benchmark:
```
./benchpress_cli.py -b system install syscall_single_core
```

Single core benchmark:
```
./benchpress_cli.py -b system run syscall_single_core
```

Multi core (use all available cores):
```
./benchpress_cli.py -b system run syscall_autoscale
```

The syscall benchmark supports the following arguments:

```
./benchpress_cli.py -b system run syscall_single_core \
    -i '{"workers": 1, "duration_s": 60, "nanosleep_ns": 100, "base_port": 16500, "syscalls": "getpid,nanosleep,tcp"}'
```
Where:
- `workers`: Number of threads to use. -1 to use all cores
- `duration_s`: Test duration in seconds
- `nanosleep_ns`: Duration of nanosleep in nanoseconds
- `base_port`: Base port for TCP server
- `syscalls`: Comma separated list of syscalls to test. All if empty

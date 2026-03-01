# Type Conversion Benchmark
`type_conversion` benchmark measures the performance of converting data between different types:
- float32
- float16
- bfloat16
- int8

## Installation
To install the benchmark, execute the following command:
```bash
./benchpress -b ai install type_conversion
```

## Run Type Conversion Benchmark
To run the benchmark, please use following command
```bash
./benchpress -b ai run type_conversion
```

## Reporting and Measurement
After the type conversion benchmark finished, benchpress will report the
results in JSON format like the following:

```json
  "benchmarks": [
    {
      "name": "FPTypeConv/fp32_to_fp16_neon",
      "family_index": 0,
      "per_family_instance_index": 0,
      "run_name": "FPTypeConv/fp32_to_fp16_neon",
      "run_type": "iteration",
      "repetitions": 1,
      "repetition_index": 0,
      "threads": 1,
      "iterations": 2263486,
      "real_time": 3.0994000141077646e+02,
      "cpu_time": 3.0927213687206364e+02,
      "time_unit": "ns",
      "elem/s": 1.3243999415616249e+10
    },
    {
      "name": "FPTypeConv/fp16_to_fp32_neon",
      "family_index": 1,
      "per_family_instance_index": 0,
      "run_name": "FPTypeConv/fp16_to_fp32_neon",
      "run_type": "iteration",
      "repetitions": 1,
      "repetition_index": 0,
      "threads": 1,
      "iterations": 2269399,
      "real_time": 3.0921412761145677e+02,
      "cpu_time": 3.0842751230612146e+02,
      "time_unit": "ns",
      "elem/s": 1.3280267928675003e+10
    },
  ]
```

# Performance Scripts for Different Architectures
We provide performance scripts tailored for various architectures. These scripts are designed to collect real-time micro-architecture metrics during benchmark execution. After data collection, processing scripts are available to convert the data into a unified format for analysis.

In addition to DCPerf, these scripts can be used in a standalone way. A typical flow is first `./collect_[arch]_perf_counter.sh > ./perf.txt` for several minutes, and then `./generate_[cpu]_report.py ./perf.txt` to see the results.

# Supported Architectures
## AMD Zen3
- Data Collection: collect_amd_perf_counter.sh
- Data Processing: generate_amd_perf_report.py --arch zen3
## AMD Zen4
- Data Collection:
collect_amd_perf_counter.sh
collect_amd_zen4_perf_counters.sh
- Data Processing: generate_amd_perf_report.py --arch zen4
## AMD Zen5
- Data Collection: collect_amd_zen5_perf_counters.sh
- Data Processing: generate_amd_perf_report.py --arch zen5
## AMD Zen5 Engineer Samples
- Data Collection: collect_amd_zen5_perf_counters.sh
- Data Processing: generate_amd_perf_report.py --arch zen5es
## ARM (NVIDIA Grace)
- Data Collection: collect_nvda_neoversev2_perf_counters.sh
- Data Processing: generate_arm_perf_report.py
## ARM (Other)

Use [topdown tool](https://learn.arm.com/install-guides/topdown-tool/).

#!/bin/bash
gets=5
sets=1
reqs=2000000
threads=8
threads_db=1
threads_coh=0
clients=10
clients_coh=1
kmin=1
kmax=1457025
kbytes=32
server=localhost #rtptest51075.frc2
cert='/home/vandrei/work/benchmarking/dev_tao_bench/certs_client/01-memtier.pem'
key='/home/vandrei/work/benchmarking/dev_tao_bench/certs_client/01-memtier.key'
run_cmd='buck run @mode/opt-lto :memtier_client_bench --'
#run_cmd='./memtier-benchmark-12182019/memtier_benchmark'

$run_cmd --show-config -s $server -p 11211 -P memcache_binary --cert="$cert" --key="$key" \
--tls --tls-skip-verify --key-pattern=R:R --distinct-client-seed --randomize -R --hide-histogram \
--expiry-range=1800-10800 --data-size-range=128-1024 --ratio=$sets:$gets --key-minimum=$kmin \
--key-bytes=$kbytes --key-maximum=$kmax -t $threads --clients=$clients -n $reqs \
--threads-db=$threads_db --threads-coherence=$threads_coh --clients-coherence=$clients_coh

#buck run memtier-benchmark-12182019:memtier_client_tao_bench -- --show-config -s localhost -p 11211 \
#-P memcache_binary --key-pattern=R:R --distinct-client-seed --randomize -R --hide-histogram \
#--expiry-range=1800-10800 --data-size-range=64-704 --ratio=$sets:$gets --key-minimum=$kmin \
#--key-maximum=$kmax -t $threads --clients=1 -n $reqs

# --show-config -s localhost -p 11211 -P memcache_binary
# --cert="/home/vandrei/work/benchmarking/dev_tao_bench/certs_client/01-memtier.pem"
# --key="/home/vandrei/work/benchmarking/dev_tao_bench/certs_client/01-memtier.key"
# --tls --tls-skip-verify --key-pattern=R:R --distinct-client-seed --randomize -R
# --hide-histogram --expiry-range=1800-10800 --data-size-range=64-704 --ratio=1:1
# --key-minimum=1 --key-maximum=1000 -t 1 --clients=1 -n 100 --threads-db=1 --threads-coherence=1

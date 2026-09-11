# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

"""Smoke test for the PairRole::RequestResponse chunkOp RPC.

Configures a running HalcyonServiceMain with pair_role=RequestResponse so the
server opens per-mount ChunkMappers + fileset FDs, then issues chunkOp reads
for a sweep of chunkIds. A "Success + payload>0" proves the whole RR path
(RPC handler -> ChunkMapper lookup -> real pread from fileset -> IOBuf back
over the wire) works end-to-end.
"""

import asyncio
import sys

from cea.halcyon.py3.halcyon.thrift_clients import HalcyonService
from cea.halcyon.py3.halcyon.thrift_types import (
    BenchmarkConfiguration,
    ChunkOpRequest,
    FileLayout,
    IoSizeRange,
    NodeConfig,
    Operation,
    PairRole,
    RuntimeConfiguration,
)
from folly.iobuf import IOBuf
from thrift.python.client import get_client


MOUNTS = [f"/mnt/d{i}" for i in range(6)]
FILE_LAYOUT = FileLayout(maxFileSize=8462336, dirs=10, subdirs=1, filesPerDir=100)
READ_SIZES = [IoSizeRange(text="4K", lowerBound=4096, upperBound=4096, percentage=100)]
WRITE_SIZES = [IoSizeRange(text="4K", lowerBound=4096, upperBound=4096, percentage=100)]


def _build_config() -> BenchmarkConfiguration:
    rt = RuntimeConfiguration(
        warmupTime=0,
        runTime=1,
        reportingInterval=1,
        qps=0,
        threadsPerDisk=1,
        networkThreads=1,
        numMountpoints=len(MOUNTS),
        doDirectIo=False,
        doFileIo=False,
        doNetworkIo=False,
        doCacheIo=False,
    )
    node = NodeConfig(partner="localhost", role=PairRole.RequestResponse)
    return BenchmarkConfiguration(
        readPercentage=1.0,
        readSizes=READ_SIZES,
        writeSizes=WRITE_SIZES,
        mergedSizes=READ_SIZES,
        mountpoints=MOUNTS,
        fileLayout=FILE_LAYOUT,
        runtimeConfig=rt,
        nodeConfig=node,
    )


async def _run(host: str, port: int, chunk_ids: list[int]) -> int:
    async with get_client(HalcyonService, host=host, port=port) as client:
        print(f"connected to {host}:{port}")

        cfg = _build_config()
        resp = await client.setBenchmarkConfiguration(cfg)
        print(
            f"setBenchmarkConfiguration -> rc={resp.return_code} msg={resp.message!r}"
        )
        if str(resp.return_code) != "HalcyonReturnCode.Success":
            print("FAIL: config rejected -- see daemon log")
            return 1

        successes = 0
        failures = 0
        for cid in chunk_ids:
            req = ChunkOpRequest(
                op=Operation.Read, chunkId=cid, size=4096, payload=IOBuf(b"")
            )
            r = await client.chunkOp(req)
            plen = r.payload.chain_size() if r.payload else 0
            tag = (
                "OK  "
                if str(r.return_code) == "HalcyonReturnCode.Success" and plen > 0
                else "MISS"
            )
            print(f"  {tag} chunkId={cid} -> rc={r.return_code} payload={plen} bytes")
            if tag == "OK  ":
                successes += 1
            else:
                failures += 1

    print(
        f"summary: {successes} success, {failures} miss/error, {len(chunk_ids)} total"
    )
    return 0 if successes > 0 else 2


def main() -> None:
    host = sys.argv[1] if len(sys.argv) > 1 else "localhost"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 23459
    if len(sys.argv) > 3:
        chunk_ids = [int(x) for x in sys.argv[3].split(",")]
    else:
        chunk_ids = list(range(0, 20)) + [100, 1000, 10000, 100000, 1000000]
    sys.exit(asyncio.run(_run(host, port, chunk_ids)))


if __name__ == "__main__":
    main()

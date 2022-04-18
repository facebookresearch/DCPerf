#!/usr/bin/env python3
# Copyright (c) 2017-present, Facebook, Inc.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree. An additional grant
# of patent rights can be found in the PATENTS file in the same directory.

from .benchdnn import BenchdnnParser
from .cachebench import CacheBenchParser
from .checkmark import CheckmarkParser
from .clang import ClangParser
from .cloudsuite_graph import CloudSuiteGraphParser
from .compression_parser import CompressionParser
from .django_workload import DjangoWorkloadParser
from .encryption import EncryptionParser
from .fb_fiosynth import Fiosynth_Parser
from .fbgemm import FbgemmParser
from .feedsim import FeedSimParser
from .fio import FioParser
from .gapbs import GAPBSParser
from .generic import JSONParser
from .graph500 import Graph500Parser
from .iperf import IperfParser
from .ltp import LtpParser
from .memcached_bench import MemcachedBenchParser
from .minebench import KMeansParser
from .minebench import PLSAParser
from .minebench import RSearchParser
from .mlc import MlcParser
from .multichase_fairness import MultichaseFairnessParser
from .multichase_pingpong import MultichasePingpongParser
from .multichase_pointer import MultichasePointerParser
from .nginx_wrk_bench import NginxWrkParser
from .nnpi_net4 import NNPINet4Parser
from .returncode import ReturncodeParser
from .schbench import SchbenchParser
from .sigrid import SigridParser
from .silo import SiloParser
from .small_locks_bench import SmallLocksParser
from .spark_standalone import SparkStandaloneParser
from .spec_cpu2006 import SPECCPU2006Parser
from .stream import StreamParser
from .tailbench import TailBenchParser
from .tao_bench import TaoBenchParser


def register_parsers(factory):
    factory.register("benchdnn", BenchdnnParser)
    factory.register("clang", ClangParser)
    factory.register("compression_parser", CompressionParser)
    factory.register("django_workload", DjangoWorkloadParser)
    factory.register("encryption", EncryptionParser)
    factory.register("fb_fiosynth", Fiosynth_Parser)
    factory.register("fbgemm", FbgemmParser)
    factory.register("fio", FioParser)
    factory.register("gapbs", GAPBSParser)
    factory.register("graph500", Graph500Parser)
    factory.register("json", JSONParser)
    factory.register("ltp", LtpParser)
    factory.register("nginx_wrk_bench", NginxWrkParser)
    factory.register("minebench_kmeans", KMeansParser)
    factory.register("minebench_plsa", PLSAParser)
    factory.register("minebench_rsearch", RSearchParser)
    factory.register("multichase_pointer", MultichasePointerParser)
    factory.register("multichase_pingpong", MultichasePingpongParser)
    factory.register("multichase_fairness", MultichaseFairnessParser)
    factory.register("returncode", ReturncodeParser)
    factory.register("schbench", SchbenchParser)
    factory.register("silo", SiloParser)
    factory.register("sigrid", SigridParser)
    factory.register("small_locks_bench", SmallLocksParser)
    factory.register("spark_standalone", SparkStandaloneParser)
    factory.register("memcached_bench", MemcachedBenchParser)
    factory.register("cachebench", CacheBenchParser)
    factory.register("tao_bench", TaoBenchParser)
    factory.register("speccpu2006", SPECCPU2006Parser)
    factory.register("stream", StreamParser)
    factory.register("mlc", MlcParser)
    factory.register("iperf", IperfParser)
    factory.register("checkmark", CheckmarkParser)
    factory.register("nnpi_net4", NNPINet4Parser)
    factory.register("feedsim", FeedSimParser)
    factory.register("tailbench_imgdnn", TailBenchParser)
    factory.register("cloudsuite_graph", CloudSuiteGraphParser)

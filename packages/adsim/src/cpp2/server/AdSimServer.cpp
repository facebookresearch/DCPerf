/*
 * Copyright (c) Facebook, Inc. and its affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <algorithm>
#include <fstream>
#include <streambuf>
#include <string>

#include <cea/chips/adsim/cpp2/server/AdSimService.h>
#include <cea/chips/adsim/cpp2/server/dwarfs/Dwarfs.h>
#include <folly/init/Init.h>
#include <folly/io/async/test/TestSSLServer.h>
#include <folly/json/json.h>
#include <folly/portability/GFlags.h>
#include <glog/logging.h>
#include <thrift/lib/cpp2/server/Cpp2Worker.h>
#include <thrift/lib/cpp2/server/ThriftProcessor.h>
#include <thrift/lib/cpp2/server/ThriftServer.h>

DEFINE_string(config_file, "", "A json file of configurations");
DEFINE_int32(port, -1, "Overwrite port in the config file");
DEFINE_int32(num_req_handle_threads, -1, "Overwrite num_req_handle_threads");
DEFINE_int32(num_io_handle_threads, -1, "Overwrite num_io_handle_threads");
DEFINE_int32(num_app_logic_threads, -1, "Overwrite num_app_logic_threads");
DEFINE_int32(num_req_local_threads, -1, "Overwrite num_req_local_threads");
DEFINE_int32(
    num_thrift_accept_threads,
    -1,
    "Overwrite num_thrift_accept_threads");
DEFINE_string(
    kernels,
    "",
    "Overwrite kernel configuration in the config file:"
    "[{\"index\": idx, \"name\": \"kernel name\", \"pool\": \"pool name\","
    "\"stage\": stage, \"params\": {...}}, ...] "
    "(Note: only overwrite any given field of the kernel specified by the "
    "index, the index of the kernel in the Kernels array in the config file. "
    "no index field then insert a new kernel, while only index field deletes "
    "the kernel)");
DEFINE_string(tlskey, "tests-key.pem", "Path to TLS key.pem");
DEFINE_string(tlscert, "tests-cert.pem", "Path to TLS cert.pem");
DEFINE_bool(
    use_stop_tls,
    false,
    "Use stop TLS mode (transition to plaintext) instead of full TLS encryption");

using apache::thrift::DefaultThriftAcceptorFactory;
using apache::thrift::ThriftServer;
using apache::thrift::ThriftServerAsyncProcessorFactory;
using apache::thrift::ThriftTlsConfig;

using facebook::cea::chips::adsim::AdSimGlobalObjs;
using facebook::cea::chips::adsim::AdSimHandler;
using facebook::cea::chips::adsim::KERNEL_DICT;
using facebook::cea::chips::adsim::KernelConfig;
using facebook::cea::chips::adsim::Stage;

namespace {

/* The structure carries all the information needed to config the server. */
struct AdSimServerConfig {
  int port;
  int num_req_handle_threads, num_io_handle_threads, num_thrift_accept_threads;
  int num_app_logic_threads, num_req_local_threads;
  std::shared_ptr<folly::Executor> thrift_cpu_threadpool;
  std::shared_ptr<folly::Executor> thrift_io_threadpool;
  std::shared_ptr<folly::Executor> app_cpu_threadpool;
  std::vector<KernelConfig> kernels;
};

} // namespace

/* Parse config file and substitute with command line if any
 *
 * @param  $config_file  The path to the config file
 * @return  A AdSimServerConfig structure with most information populated.
 */
std::unique_ptr<AdSimServerConfig> read_config(std::string config_file) {
  std::unique_ptr<AdSimServerConfig> server_config =
      std::make_unique<AdSimServerConfig>();

  // Load and parse the config file
  std::ifstream fi(config_file);
  if (fi.is_open()) {
    // read file as a string
    std::string config_str(
        (std::istreambuf_iterator<char>(fi)), std::istreambuf_iterator<char>());
    fi.close();

    // parse string with json engine
    folly::json::serialization_opts opts;
    opts.allow_non_string_keys = true;
    const folly::dynamic& config_d = folly::parseJson(config_str, opts);

    server_config->port =
        (0 < FLAGS_port) ? FLAGS_port : config_d["port"].asInt();
    server_config->num_req_handle_threads = (0 <= FLAGS_num_req_handle_threads)
        ? FLAGS_num_req_handle_threads
        : config_d.getDefault("num_req_handle_threads", 8).asInt();
    server_config->num_io_handle_threads = (0 <= FLAGS_num_io_handle_threads)
        ? FLAGS_num_io_handle_threads
        : config_d.getDefault("num_io_handle_threads", 8).asInt();
    server_config->num_thrift_accept_threads =
        (0 <= FLAGS_num_thrift_accept_threads)
        ? FLAGS_num_thrift_accept_threads
        : config_d.getDefault("num_thrift_accept_threads", 1).asInt();
    server_config->num_app_logic_threads = (0 <= FLAGS_num_app_logic_threads)
        ? FLAGS_num_app_logic_threads
        : config_d.getDefault("num_app_logic_threads", 8).asInt();
    server_config->num_req_local_threads = (0 <= FLAGS_num_req_local_threads)
        ? FLAGS_num_req_local_threads
        : config_d.getDefault("num_req_local_threads", 8).asInt();
    // Kernel config from the config file
    {
      const folly::dynamic& kernels_d =
          config_d.getDefault("Kernels", folly::dynamic::array());
      if (kernels_d.isArray() && !kernels_d.empty()) {
        for (const auto& kernel : kernels_d) {
          if (0 == kernel.count("name")) {
            LOG(WARNING) << "Unknown kernel";
            continue;
          }
          server_config->kernels.emplace_back(
              kernel["name"].asString(),
              kernel.getDefault("pool", "defaultPool").asString(),
              kernel.getDefault("stage", -1).asInt(),
              kernel.getDefault("fanout", 1).asInt(),
              kernel.getDefault("params", folly::dynamic::object()));
        }
      }
    }
    // Kernel config from the command line
    if (2 < FLAGS_kernels.size()) {
      const folly::dynamic& kernels_d = folly::parseJson(FLAGS_kernels, opts);
      if (kernels_d.isArray() && !kernels_d.empty()) {
        for (const auto& kernel : kernels_d) {
          const int idx =
              kernel.getDefault("index", server_config->kernels.size()).asInt();
          if (idx == server_config->kernels.size() &&
              0 != kernel.count("name")) { // insert a new kernel
            server_config->kernels.emplace_back(
                kernel["name"].asString(),
                kernel.getDefault("pool", "defaultPool").asString(),
                kernel.getDefault("stage", -1).asInt(),
                kernel.getDefault("fanout", 1).asInt(),
                kernel.getDefault("params", folly::dynamic::object()));
          } else if (1 == kernel.size()) { // delete the kernel
            server_config->kernels[idx].deleted = true;
          } else { // overwrites kernel config
            server_config->kernels[idx].name =
                kernel.getDefault("name", server_config->kernels[idx].name)
                    .asString();
            server_config->kernels[idx].pool =
                kernel.getDefault("pool", server_config->kernels[idx].pool)
                    .asString();
            server_config->kernels[idx].stage =
                kernel.getDefault("stage", server_config->kernels[idx].stage)
                    .asInt();
            server_config->kernels[idx].fanout =
                kernel.getDefault("fanout", server_config->kernels[idx].fanout)
                    .asInt();
            server_config->kernels[idx].params_d = kernel.getDefault(
                "params", server_config->kernels[idx].params_d);
          }
        }
      }
    }
    LOG(INFO) << "AdSim Server port: " << server_config->port
              << ", #thriftCPU: " << server_config->num_req_handle_threads
              << ", #thriftIO: " << server_config->num_io_handle_threads
              << ", #appCPU: " << server_config->num_app_logic_threads
              << ", #reqLocal: " << server_config->num_req_local_threads;
  } else {
    LOG(ERROR) << "Failed to open configuration file " << config_file;
  }

  return server_config;
}

/* Based on the configuration to build the pipeline
 *
 * @param  $server_config  Server configuration
 * @return  A pipeline of stages
 */
std::shared_ptr<std::vector<Stage>> config_stages(
    std::unique_ptr<AdSimServerConfig> server_config) {
  std::shared_ptr<std::vector<Stage>> stages =
      std::make_shared<std::vector<Stage>>();
  for (const auto& kernel : server_config->kernels) {
    if (kernel.deleted) { // Skip those kernels marked deleted
      continue;
    }
    for (int fidx = 0; fidx < kernel.fanout; ++fidx) {
      int kernel_stage = kernel.stage;
      if (-1 == kernel_stage) { // -1 means did not specify, put at the end
        kernel_stage = stages->size();
        stages->resize(kernel_stage + 1);
      } else if (stages->size() <= kernel_stage) {
        stages->resize(kernel_stage + 1);
      }
      // Find the corredponding threadpool for each kernel
      if ("requestHandle" == kernel.pool) {
        (*stages)[kernel_stage].pools.emplace_back(
            server_config->thrift_cpu_threadpool);
      } else if ("applicationLogic" == kernel.pool) {
        (*stages)[kernel_stage].pools.emplace_back(
            server_config->app_cpu_threadpool);
      } else if ("IOHandle" == kernel.pool) {
        (*stages)[kernel_stage].pools.emplace_back(
            server_config->thrift_io_threadpool);
      } else {
        (*stages)[kernel_stage].pools.emplace_back(nullptr);
      }
      auto params_d = kernel.params_d;
      if (0 != params_d.count("input")) {
        std::string input_str = params_d["input"].asString();
        size_t idx = 0;
        idx = input_str.find("{fidx}", idx);
        while (std::string::npos != idx) {
          input_str.replace(idx, 6, std::to_string(fidx));
          idx = input_str.find("{fidx}", idx);
        }
        params_d["input"] = input_str;
      }
      if (0 != params_d.count("output")) {
        std::string output_str = params_d["output"].asString();
        size_t idx = 0;
        idx = output_str.find("{fidx}", idx);
        while (std::string::npos != idx) {
          output_str.replace(idx, 6, std::to_string(fidx));
          idx = output_str.find("{fidx}", idx);
        }
        params_d["output"] = output_str;
      }
      // Instantiate kernel object using their registered config parser
      (*stages)[kernel_stage].kernels.push_back(
          KERNEL_DICT.at(kernel.name)(params_d));
    }
  }
  LOG(INFO) << "Pipeline #stages: " << stages->size() << ", #kernels: "
            << std::accumulate(
                   std::next(stages->begin()),
                   stages->end(),
                   std::to_string(stages->at(0).kernels.size()),
                   [](std::string s, const Stage& stage) {
                     return std::move(s) + "-" +
                         std::to_string(stage.kernels.size());
                   });
  return stages;
}

/* Setup TLS for the ThriftServer
 */
void setup_tls(std::shared_ptr<ThriftServer> server) {
  server->setSSLPolicy(apache::thrift::SSLPolicy::REQUIRED);

  auto sslConfig = std::make_shared<wangle::SSLContextConfig>();
  sslConfig->setNextProtocols({"rs"});
  sslConfig->setCertificate(FLAGS_tlscert, FLAGS_tlskey, "");
  sslConfig->clientVerification =
      folly::SSLContext::VerifyClientCertificate::DO_NOT_REQUEST;
  server->setSSLConfig(std::move(sslConfig));

  ThriftTlsConfig thriftConfig;
  thriftConfig.enableThriftParamsNegotiation = true;
  thriftConfig.enableStopTLS = FLAGS_use_stop_tls;
  server->setThriftConfig(thriftConfig);
  server->setAcceptorFactory(
      std::make_shared<DefaultThriftAcceptorFactory>(server.get()));
}

/* Create a new thrift server and config it
 *
 * @param  server_config  Server configuration (threadpools not populated yet)
 * @return  A thrift server
 */
template <typename ServiceHandler>
std::shared_ptr<ThriftServer> newServer(
    std::unique_ptr<AdSimServerConfig> server_config) {
  // Create a Thrift Server
  auto server = std::make_shared<ThriftServer>();

  AdSimGlobalObjs::get()->set_shared_ptr("Gserver", server);

  server->setPort(server_config->port);
  server->setNumCPUWorkerThreads(server_config->num_req_handle_threads);
  server->setNumIOWorkerThreads(server_config->num_io_handle_threads);
  server->setNumAcceptThreads(server_config->num_thrift_accept_threads);
  server->setupThreadManager(); // manually invoke earlier to get the cpu pool
  server->setQueueTimeout(std::chrono::milliseconds(10000));

  // Populate thread pools in the server configuration before build pipeline
  server_config->app_cpu_threadpool =
      std::make_shared<folly::CPUThreadPoolExecutor>(
          server_config->num_app_logic_threads,
          std::make_shared<folly::NamedThreadFactory>("applicationLogic"));
  server_config->thrift_cpu_threadpool = server->getThreadManager();
  server_config->thrift_io_threadpool = server->getIOThreadPool();

  auto handler =
      std::make_shared<ServiceHandler>(config_stages(std::move(server_config)));
  auto proc_factory =
      std::make_shared<ThriftServerAsyncProcessorFactory<ServiceHandler>>(
          handler);

  server->setInterface(proc_factory);
  setup_tls(server);

  return server;
}

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv);

  std::unique_ptr<AdSimServerConfig> server_config =
      read_config(FLAGS_config_file);

  auto adsim_server = newServer<AdSimHandler>(std::move(server_config));
  adsim_server->serve();

  return 0;
}

// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <folly/Portability.h>

#if FOLLY_HAS_COROUTINES

#include <memory>
#include <string>
#include <vector>

#include <thrift/lib/cpp/TProcessorEventHandler.h>
#include <thrift/lib/cpp2/server/ServerModule.h>
#include <thrift/lib/cpp2/server/ServiceInterceptor.h>

namespace facebook::ucachebench {

class UcacheBenchServer;

// Production ucache installs the Service Authorization Platform, which adapts
// its authentication, connection-attribute, authorization, auditing and
// metrics handlers into Thrift ServiceInterceptors. A CPU profile of the
// production shadow tier shows a ServiceInterceptor frame on 10.1% of samples
// and ContextStack on 5.3%; ucachebench had 0.03% and 0.08%.
//
// SAP itself cannot be linked here: it lives in core_infra_security, and the
// open-source DCPerf build compiles these same sources against GitHub folly /
// fizz / fbthrift. What is portable is the interceptor framework, so the
// per-request authorization work ucachebench already performed inline is
// relocated onto it. That reproduces the real lifecycle — request-scoped state
// construction and teardown, ContextStack population, the shared_ptr and
// RequestContext traffic interceptors generate — without inventing new work:
// the operations are the same ones UcacheBenchServer used to run inline, and
// they are removed from there so nothing is counted twice.
struct UcacheBenchRequestAuthState {
  // Kept small deliberately. fbthrift stores request state inline up to 64
  // bytes; spilling past that would add heap traffic production does not have.
  uint64_t identityHash{0};
  uint32_t aclCategory{0};
};

class UcacheBenchAuthInterceptor
    : public apache::thrift::ServiceInterceptor<UcacheBenchRequestAuthState> {
 public:
  explicit UcacheBenchAuthInterceptor(UcacheBenchServer* server)
      : server_(server) {}

  std::string getName() const override {
    return "UcacheBenchAuth";
  }

  folly::coro::Task<std::optional<UcacheBenchRequestAuthState>> onRequest(
      folly::Unit*,
      apache::thrift::ServiceInterceptorBase::RequestInfo) override;

  folly::coro::Task<void> onResponse(
      UcacheBenchRequestAuthState*,
      folly::Unit*,
      apache::thrift::ServiceInterceptorBase::ResponseInfo) override;

 private:
  UcacheBenchServer* server_;
};

// Production also installs legacy TProcessorEventHandlers (ServiceRouter
// logging and the Artillery low-level handler). Those are what cause Thrift to
// build a ContextStack per request: the profile shows ContextStack on 5.3% of
// production samples and 0.1% of ours, because with zero legacy handlers Thrift
// skips the whole structure. One handler is enough to activate that lifecycle;
// its callbacks carry the per-request timestamp and audit work the handler was
// already doing rather than adding new work.
class UcacheBenchLoggingHandler
    : public apache::thrift::TProcessorEventHandler {
 public:
  void* getServiceContext(
      std::string_view /*service_name*/,
      std::string_view /*fn_name*/,
      apache::thrift::server::TConnectionContext* /*connectionContext*/)
      override;
  void freeContext(void* ctx, std::string_view fn_name) override;
  void preRead(void* ctx, std::string_view fn_name) override;
  void preWrite(void* ctx, std::string_view fn_name) override;
};

// ServerModule is how a ThriftServer takes ownership of interceptors, and is
// also the path production uses via SAP's InterceptorBasedServerModule.
class UcacheBenchServerModule : public apache::thrift::ServerModule {
 public:
  explicit UcacheBenchServerModule(UcacheBenchServer* server)
      : server_(server) {}

  std::string getName() const override {
    return "UcacheBenchServerModule";
  }

  std::vector<std::shared_ptr<apache::thrift::ServiceInterceptorBase>>
  getServiceInterceptors() override {
    return {std::make_shared<UcacheBenchAuthInterceptor>(server_)};
  }

  std::vector<std::shared_ptr<apache::thrift::TProcessorEventHandler>>
  getLegacyEventHandlers() override {
    return {std::make_shared<UcacheBenchLoggingHandler>()};
  }

 private:
  UcacheBenchServer* server_;
};

} // namespace facebook::ucachebench

#endif // FOLLY_HAS_COROUTINES

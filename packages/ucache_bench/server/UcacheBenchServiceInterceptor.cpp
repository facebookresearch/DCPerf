// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#include "UcacheBenchServiceInterceptor.h"

#if FOLLY_HAS_COROUTINES

#include <folly/BenchmarkUtil.h>
#include <chrono>

#include "UcacheBenchServer.h"

namespace facebook::ucachebench {

folly::coro::Task<std::optional<UcacheBenchRequestAuthState>>
UcacheBenchAuthInterceptor::onRequest(
    folly::Unit*,
    apache::thrift::ServiceInterceptorBase::RequestInfo info) {
  auto result = server_->runRequestAuthorization(info.methodName);
  co_return UcacheBenchRequestAuthState{
      result.identityHash, result.aclCategory};
}

folly::coro::Task<void> UcacheBenchAuthInterceptor::onResponse(
    UcacheBenchRequestAuthState* state,
    folly::Unit*,
    apache::thrift::ServiceInterceptorBase::ResponseInfo) {
  // Production audits on the response path, after the handler has run, so the
  // exit-side work is kept here rather than folded into onRequest.
  server_->runResponseAudit(state != nullptr ? state->identityHash : 0);
  co_return;
}

namespace {
// Per-request context for the legacy handler. Production's ServiceRouter
// logging handler stamps request entry and reports on write; this carries the
// same shape so ContextStack allocates and frees a real object per request.
struct LoggingContext {
  std::chrono::steady_clock::time_point start;
};
} // namespace

void* UcacheBenchLoggingHandler::getServiceContext(
    std::string_view,
    std::string_view,
    apache::thrift::server::TConnectionContext*) {
  return new LoggingContext{std::chrono::steady_clock::now()};
}

void UcacheBenchLoggingHandler::freeContext(void* ctx, std::string_view) {
  delete static_cast<LoggingContext*>(ctx);
}

void UcacheBenchLoggingHandler::preRead(void* ctx, std::string_view) {
  if (ctx != nullptr) {
    folly::doNotOptimizeAway(static_cast<LoggingContext*>(ctx)->start);
  }
}

void UcacheBenchLoggingHandler::preWrite(void* ctx, std::string_view) {
  if (ctx == nullptr) {
    return;
  }
  auto elapsed = std::chrono::steady_clock::now() -
      static_cast<LoggingContext*>(ctx)->start;
  folly::doNotOptimizeAway(elapsed);
}

} // namespace facebook::ucachebench

#endif // FOLLY_HAS_COROUTINES

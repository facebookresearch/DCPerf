/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UcacheBenchOnRequest.h"

#include <folly/io/IOBuf.h>

#include "UcacheBenchRequestCommon.h"
namespace facebook {
namespace ucachebench {

UcacheBenchOnRequest::UcacheBenchOnRequest(
    std::shared_ptr<UcacheBenchServer> server)
    : server_(server) {}

void UcacheBenchOnRequest::onRequestThrift(
    apache::thrift::HandlerCallbackPtr<UcbGetReply> callback,
    UcbGetRequest&& request) {
  ucacheBenchOnRequestCommon(
      std::move(callback), std::move(request), [this](auto&& cb, auto&& req) {
        cb->result(server_->processUcbGetSync(req));
      });
}

void UcacheBenchOnRequest::onRequestThrift(
    apache::thrift::HandlerCallbackPtr<UcbSetReply> callback,
    UcbSetRequest&& request) {
  ucacheBenchOnRequestCommon(
      std::move(callback), std::move(request), [this](auto&& cb, auto&& req) {
        cb->result(server_->processUcbSetSync(req));
      });
}

void UcacheBenchOnRequest::onRequestThrift(
    apache::thrift::HandlerCallbackPtr<UcbDeleteReply> callback,
    UcbDeleteRequest&& request) {
  ucacheBenchOnRequestCommon(
      std::move(callback), std::move(request), [this](auto&& cb, auto&& req) {
        cb->result(server_->processUcbDeleteSync(req));
      });
}

void UcacheBenchOnRequest::onRequestThrift(
    apache::thrift::HandlerCallbackPtr<facebook::memcache::McVersionReply>
        callback,
    facebook::memcache::McVersionRequest&& request) {
  ucacheBenchOnRequestCommon(
      std::move(callback), std::move(request), [](auto&& cb, auto&& /* req */) {
        facebook::memcache::McVersionReply reply;
        reply.result() = carbon::Result::FOUND;
        reply.value() =
            *folly::IOBuf::copyBuffer("UcacheBench 1.0 (with Fiber support)");
        cb->result(std::move(reply));
      });
}

} // namespace ucachebench
} // namespace facebook

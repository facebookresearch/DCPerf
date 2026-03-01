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
        try {
          auto reply = server_->processUcbGet(req).get();
          cb->result(std::move(reply));
        } catch (const std::exception& ex) {
          UcbGetReply reply;
          reply.result_ref() = carbon::Result::REMOTE_ERROR;
          reply.message_ref() = ex.what();
          cb->result(std::move(reply));
        }
      });
}

void UcacheBenchOnRequest::onRequestThrift(
    apache::thrift::HandlerCallbackPtr<UcbSetReply> callback,
    UcbSetRequest&& request) {
  ucacheBenchOnRequestCommon(
      std::move(callback), std::move(request), [this](auto&& cb, auto&& req) {
        try {
          auto reply = server_->processUcbSet(req).get();
          cb->result(std::move(reply));
        } catch (const std::exception& ex) {
          UcbSetReply reply;
          reply.result_ref() = carbon::Result::REMOTE_ERROR;
          reply.message_ref() = ex.what();
          cb->result(std::move(reply));
        }
      });
}

void UcacheBenchOnRequest::onRequestThrift(
    apache::thrift::HandlerCallbackPtr<UcbDeleteReply> callback,
    UcbDeleteRequest&& request) {
  ucacheBenchOnRequestCommon(
      std::move(callback), std::move(request), [this](auto&& cb, auto&& req) {
        try {
          auto reply = server_->processUcbDelete(req).get();
          cb->result(std::move(reply));
        } catch (const std::exception& ex) {
          UcbDeleteReply reply;
          reply.result_ref() = carbon::Result::REMOTE_ERROR;
          reply.message_ref() = ex.what();
          cb->result(std::move(reply));
        }
      });
}

void UcacheBenchOnRequest::onRequestThrift(
    apache::thrift::HandlerCallbackPtr<facebook::memcache::McVersionReply>
        callback,
    facebook::memcache::McVersionRequest&& request) {
  ucacheBenchOnRequestCommon(
      std::move(callback), std::move(request), [](auto&& cb, auto&& /* req */) {
        facebook::memcache::McVersionReply reply;
        reply.result_ref() = carbon::Result::FOUND;
        reply.value_ref() =
            *folly::IOBuf::copyBuffer("UcacheBench 1.0 (with Fiber support)");
        cb->result(std::move(reply));
      });
}

} // namespace ucachebench
} // namespace facebook

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "UcacheBenchCommandHandler.h"

#include <folly/futures/Future.h>
#include <folly/io/IOBuf.h>

#include "cea/chips/benchpress/packages/ucache_bench/protocol/gen/UcacheBenchMessages.h"

namespace facebook {
namespace ucachebench {

// Singleton instance management
std::shared_ptr<UcacheBenchCommandHandler>&
UcacheBenchCommandHandler::instanceInternal() {
  static std::shared_ptr<UcacheBenchCommandHandler> instance;
  return instance;
}

UcacheBenchCommandHandler& UcacheBenchCommandHandler::instance() {
  auto& inst = instanceInternal();
  if (!inst) {
    inst = std::make_shared<UcacheBenchCommandHandler>();
  }
  return *inst;
}

std::shared_ptr<UcacheBenchCommandHandler>
UcacheBenchCommandHandler::instanceAsSharedPtr() {
  auto& inst = instanceInternal();
  if (!inst) {
    inst = std::make_shared<UcacheBenchCommandHandler>();
  }
  return inst;
}

// Direct implementation of Thrift service interface methods
void UcacheBenchCommandHandler::async_eb_ucbGet(
    apache::thrift::HandlerCallbackPtr<UcbGetReply> callback,
    const UcbGetRequest& request) {
  if (!server_) {
    UcbGetReply reply;
    reply.result_ref() = carbon::Result::REMOTE_ERROR;
    reply.message_ref() = "Server not initialized";
    callback->result(std::move(reply));
    return;
  }

  try {
    // Process the request and forward result to callback
    auto future = server_->processUcbGet(request);
    std::move(future)
        .via(callback->getEventBase())
        .thenValue(
            [callback = std::move(callback)](UcbGetReply&& reply) mutable {
              callback->result(std::move(reply));
            })
        .thenError([callback = std::move(callback)](
                       folly::exception_wrapper&& ex) mutable {
          UcbGetReply reply;
          reply.result_ref() = carbon::Result::REMOTE_ERROR;
          reply.message_ref() = folly::exceptionStr(ex);
          callback->result(std::move(reply));
        });
  } catch (const std::exception& ex) {
    UcbGetReply reply;
    reply.result_ref() = carbon::Result::REMOTE_ERROR;
    reply.message_ref() = ex.what();
    callback->result(std::move(reply));
  }
}

void UcacheBenchCommandHandler::async_eb_ucbSet(
    apache::thrift::HandlerCallbackPtr<UcbSetReply> callback,
    const UcbSetRequest& request) {
  if (!server_) {
    UcbSetReply reply;
    reply.result_ref() = carbon::Result::REMOTE_ERROR;
    reply.message_ref() = "Server not initialized";
    callback->result(std::move(reply));
    return;
  }

  try {
    // Process the request and forward result to callback
    auto future = server_->processUcbSet(request);
    std::move(future)
        .via(callback->getEventBase())
        .thenValue(
            [callback = std::move(callback)](UcbSetReply&& reply) mutable {
              callback->result(std::move(reply));
            })
        .thenError([callback = std::move(callback)](
                       folly::exception_wrapper&& ex) mutable {
          UcbSetReply reply;
          reply.result_ref() = carbon::Result::REMOTE_ERROR;
          reply.message_ref() = folly::exceptionStr(ex);
          callback->result(std::move(reply));
        });
  } catch (const std::exception& ex) {
    UcbSetReply reply;
    reply.result_ref() = carbon::Result::REMOTE_ERROR;
    reply.message_ref() = ex.what();
    callback->result(std::move(reply));
  }
}

void UcacheBenchCommandHandler::async_eb_ucbDelete(
    apache::thrift::HandlerCallbackPtr<UcbDeleteReply> callback,
    const UcbDeleteRequest& request) {
  if (!server_) {
    UcbDeleteReply reply;
    reply.result_ref() = carbon::Result::REMOTE_ERROR;
    reply.message_ref() = "Server not initialized";
    callback->result(std::move(reply));
    return;
  }

  try {
    // Process the request and forward result to callback
    auto future = server_->processUcbDelete(request);
    std::move(future)
        .via(callback->getEventBase())
        .thenValue(
            [callback = std::move(callback)](UcbDeleteReply&& reply) mutable {
              callback->result(std::move(reply));
            })
        .thenError([callback = std::move(callback)](
                       folly::exception_wrapper&& ex) mutable {
          UcbDeleteReply reply;
          reply.result_ref() = carbon::Result::REMOTE_ERROR;
          reply.message_ref() = folly::exceptionStr(ex);
          callback->result(std::move(reply));
        });
  } catch (const std::exception& ex) {
    UcbDeleteReply reply;
    reply.result_ref() = carbon::Result::REMOTE_ERROR;
    reply.message_ref() = ex.what();
    callback->result(std::move(reply));
  }
}

void UcacheBenchCommandHandler::async_eb_mcVersion(
    apache::thrift::HandlerCallbackPtr<facebook::memcache::McVersionReply>
        callback,
    const facebook::memcache::McVersionRequest& /* request */) {
  facebook::memcache::McVersionReply reply;
  reply.result_ref() = carbon::Result::FOUND;
  reply.value_ref() = *folly::IOBuf::copyBuffer("UcacheBench 1.0");
  callback->result(std::move(reply));
}

} // namespace ucachebench
} // namespace facebook

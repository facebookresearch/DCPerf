/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>

#include <thrift/lib/cpp2/async/processor/HandlerCallback.h>
#ifdef OSS_BUILD
#include "UcacheBenchMessages.h"
#include "UcacheBenchServer.h"
#else
#include "cea/chips/benchpress/packages/ucache_bench/protocol/gen/UcacheBenchMessages.h"
#include "cea/chips/benchpress/packages/ucache_bench/server/UcacheBenchServer.h"
#endif

namespace facebook {
namespace ucachebench {

/**
 * OnRequest handler for UcacheBench that handles Thrift requests
 * by forwarding them to UcacheBenchServer.
 * This follows the same pattern as production ucache server.
 */
class UcacheBenchOnRequest {
 public:
  explicit UcacheBenchOnRequest(std::shared_ptr<UcacheBenchServer> server);

  // Thrift request handlers
  void onRequestThrift(
      apache::thrift::HandlerCallbackPtr<UcbGetReply> callback,
      UcbGetRequest&& request);

  void onRequestThrift(
      apache::thrift::HandlerCallbackPtr<UcbSetReply> callback,
      UcbSetRequest&& request);

  void onRequestThrift(
      apache::thrift::HandlerCallbackPtr<UcbDeleteReply> callback,
      UcbDeleteRequest&& request);

  void onRequestThrift(
      apache::thrift::HandlerCallbackPtr<facebook::memcache::McVersionReply>
          callback,
      facebook::memcache::McVersionRequest&& request);

 private:
  std::shared_ptr<UcacheBenchServer> server_;
};

} // namespace ucachebench
} // namespace facebook

/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <folly/coro/Task.h>
#include <folly/portability/GFlags.h>

#include "proxygen/lib/http/coro/HTTPCoroSession.h"

namespace ti {
namespace foss_revproxy {

/**
 * ContentHandler - HTTP server that serves various content types
 *
 * Features:
 * - Serves HTML, JavaScript, JSON
 * - Serves embedded images (PNG)
 * - Can randomly reset connections for testing
 * - Cycles through different responses
 */
class ContentHandler : public proxygen::coro::HTTPHandler {
 public:
  explicit ContentHandler(double resetProbability = 0.0);

  // HTTPHandler interface
  folly::coro::Task<proxygen::coro::HTTPSourceHolder> handleRequest(
      folly::EventBase* evb,
      proxygen::coro::HTTPSessionContextPtr ctx,
      proxygen::coro::HTTPSourceHolder requestSource) override;

 private:
  // Content generation methods
  std::string getHTMLContent(int variant);
  std::string getJSContent(int variant);
  std::string getJSONContent();
  std::vector<uint8_t> getPNGImage(int variant);

  // Get appropriate content based on URL path
  proxygen::coro::HTTPSourceHolder generateResponse(
      const std::string& path,
      int requestNumber);

  double resetProbability_;
  std::atomic<int> requestCounter_{0};
};

} // namespace foss_revproxy
} // namespace ti

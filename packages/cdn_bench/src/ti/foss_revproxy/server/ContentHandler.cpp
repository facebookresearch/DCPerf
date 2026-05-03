/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "ti/foss_revproxy/server/ContentHandler.h"
#include <folly/logging/xlog.h>
#include <folly/portability/GFlags.h>

#include "proxygen/lib/http/coro/HTTPFixedSource.h"

DEFINE_int32(
    response_size,
    0,
    "Minimum response body size in bytes (0 = no padding)");

using namespace proxygen;
using namespace proxygen::coro;

namespace ti {
namespace foss_revproxy {

// Constants for content generation
namespace {
constexpr int NUM_HTML_VARIANTS = 3;
constexpr int NUM_JS_VARIANTS = 2;
constexpr int NUM_PNG_VARIANTS = 3;
constexpr size_t REQUEST_BODY_READ_SIZE =
    4096; // Buffer size for reading request bodies

std::unique_ptr<folly::IOBuf> padBody(std::unique_ptr<folly::IOBuf> body) {
  if (FLAGS_response_size <= 0) {
    return body;
  }
  auto currentSize = body ? body->computeChainDataLength() : 0;
  if (currentSize >= static_cast<size_t>(FLAGS_response_size)) {
    return body;
  }
  auto padding = folly::IOBuf::create(FLAGS_response_size - currentSize);
  memset(padding->writableData(), 'X', FLAGS_response_size - currentSize);
  padding->append(FLAGS_response_size - currentSize);
  if (body) {
    body->prependChain(std::move(padding));
    return body;
  }
  return padding;
}
} // namespace

// Minimal valid PNG images (1x1 pixel, different colors)
static const std::vector<uint8_t> RED_PNG = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
    0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0x00,
    0x00, 0x03, 0x01, 0x01, 0x00, 0x18, 0xDD, 0x8D, 0xB4, 0x00, 0x00, 0x00,
    0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

static const std::vector<uint8_t> GREEN_PNG = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
    0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0x60, 0xF8, 0x0F, 0x00,
    0x00, 0x02, 0x01, 0x01, 0xE5, 0x27, 0xDE, 0xFC, 0x00, 0x00, 0x00, 0x00,
    0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

static const std::vector<uint8_t> BLUE_PNG = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
    0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
    0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x77, 0x53, 0xDE, 0x00, 0x00, 0x00,
    0x0C, 0x49, 0x44, 0x41, 0x54, 0x08, 0xD7, 0x63, 0x60, 0x60, 0xF8, 0x0F,
    0x00, 0x00, 0x04, 0x01, 0x01, 0xE7, 0x87, 0xD8, 0x0A, 0x00, 0x00, 0x00,
    0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82};

ContentHandler::ContentHandler(double resetProbability)
    : resetProbability_(resetProbability) {}

folly::coro::Task<HTTPSourceHolder> ContentHandler::handleRequest(
    folly::EventBase* evb,
    HTTPSessionContextPtr ctx,
    HTTPSourceHolder requestSource) {
  // Read request headers
  auto headerEvent = co_await co_awaitTry(requestSource.readHeaderEvent());
  if (headerEvent.hasException()) {
    XLOG(ERR) << "Failed to read request headers";
    co_return HTTPFixedSource::makeFixedResponse(500, "Internal Server Error");
  }

  // Get path from headers
  std::string path = headerEvent->headers->getPath();
  std::string method = headerEvent->headers->getMethodString();

  if (!headerEvent->eom) {
    // Drain request body
    while (true) {
      auto bodyEvent =
          co_await requestSource.readBodyEvent(REQUEST_BODY_READ_SIZE);
      if (bodyEvent.eom) {
        break;
      }
    }
  }

  // Check if we should reset this connection
  thread_local std::mt19937 rng(std::random_device{}());
  std::uniform_real_distribution<double> dist(0.0, 1.0);
  if (resetProbability_ > 0 && dist(rng) < resetProbability_) {
    XLOG(INFO) << "Randomly resetting connection";
    // Return error to simulate reset
    co_return HTTPFixedSource::makeFixedResponse(503, "Connection Reset");
  }

  int reqNum = requestCounter_++;
  XLOG(DBG2) << "Request #" << reqNum << ": " << method << " " << path;

  co_return generateResponse(path, reqNum);
}

HTTPSourceHolder ContentHandler::generateResponse(
    const std::string& path,
    int requestNumber) {
  // Route based on path
  if (path == "/" || path == "/index.html") {
    auto msg = std::make_unique<HTTPMessage>();
    msg->setStatusCode(200);
    msg->setStatusMessage("OK");
    msg->getHeaders().set(HTTP_HEADER_CONTENT_TYPE, "text/html; charset=utf-8");
    return HTTPFixedSource::makeFixedSource(
        std::move(msg),
        padBody(folly::IOBuf::copyBuffer(getHTMLContent(requestNumber))));
  }

  if (path.find("/api/") == 0 || path.find(".json") != std::string::npos) {
    auto msg = std::make_unique<HTTPMessage>();
    msg->setStatusCode(200);
    msg->setStatusMessage("OK");
    msg->getHeaders().set(HTTP_HEADER_CONTENT_TYPE, "application/json");
    return HTTPFixedSource::makeFixedSource(
        std::move(msg), padBody(folly::IOBuf::copyBuffer(getJSONContent())));
  }

  if (path.find(".js") != std::string::npos) {
    auto msg = std::make_unique<HTTPMessage>();
    msg->setStatusCode(200);
    msg->setStatusMessage("OK");
    msg->getHeaders().set(HTTP_HEADER_CONTENT_TYPE, "application/javascript");
    return HTTPFixedSource::makeFixedSource(
        std::move(msg),
        padBody(folly::IOBuf::copyBuffer(getJSContent(requestNumber))));
  }

  if (path.find(".png") != std::string::npos || path.find("/image") == 0) {
    auto imgData = getPNGImage(requestNumber);
    auto msg = std::make_unique<HTTPMessage>();
    msg->setStatusCode(200);
    msg->setStatusMessage("OK");
    msg->getHeaders().set(HTTP_HEADER_CONTENT_TYPE, "image/png");
    return HTTPFixedSource::makeFixedSource(
        std::move(msg),
        padBody(folly::IOBuf::copyBuffer(imgData.data(), imgData.size())));
  }

  // Default: return 404
  auto msg = std::make_unique<HTTPMessage>();
  msg->setStatusCode(404);
  msg->setStatusMessage("Not Found");
  msg->getHeaders().set(HTTP_HEADER_CONTENT_TYPE, "text/html; charset=utf-8");
  return HTTPFixedSource::makeFixedSource(
      std::move(msg),
      folly::IOBuf::copyBuffer(
          "<html><body><h1>404 Not Found</h1></body></html>"));
}

std::string ContentHandler::getHTMLContent(int request_num) {
  int variant = request_num % NUM_HTML_VARIANTS;
  return fmt::format(
      "<!DOCTYPE html>\n<html>\n<head>\n"
      "<title>FOSS Revproxy Test Server - Page {}</title>\n"
      "<style>body {{ font-family: Arial; margin: 40px; }}</style>\n"
      "</head>\n<body>\n"
      "<h1>FOSS Revproxy Test Server</h1>\n"
      "<p>This is test page variant {}</p>\n"
      "<p>Request counter: {}</p>\n"
      "<script src=\"/app.js\"></script>\n"
      "</body>\n</html>",
      variant,
      variant,
      requestCounter_.load());
}

std::string ContentHandler::getJSContent(int request_num) {
  switch (request_num % NUM_JS_VARIANTS) {
    case 0:
      return R"(
console.log('FOSS Revproxy Test - Script A');
function testFunction() {
  return 'Hello from server script A';
}
)";
    case 1:
    default: // unnecessary but the linter wants it
      return R"(
console.log('FOSS Revproxy Test - Script B');
function testFunction() {
  return 'Hello from server script B';
}
)";
  }
}

std::string ContentHandler::getJSONContent() {
  return fmt::format(
      "{{\n"
      "  \"server\": \"foss_revproxy_test\",\n"
      "  \"requestCount\": {},\n"
      "  \"timestamp\": {},\n"
      "  \"data\": [\n"
      "    {{\"id\": 1, \"value\": \"test1\"}},\n"
      "    {{\"id\": 2, \"value\": \"test2\"}},\n"
      "    {{\"id\": 3, \"value\": \"test3\"}}\n"
      "  ]\n"
      "}}",
      requestCounter_.load(),
      time(nullptr));
}

std::vector<uint8_t> ContentHandler::getPNGImage(int variant) {
  switch (variant % NUM_PNG_VARIANTS) {
    case 0:
      return RED_PNG;
    case 1:
      return GREEN_PNG;
    case 2:
    default: // unnecessary but the linter wants it
      return BLUE_PNG;
  }
}
} // namespace foss_revproxy
} // namespace ti

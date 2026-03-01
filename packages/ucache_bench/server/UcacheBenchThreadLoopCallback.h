// (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

#pragma once

#include <folly/io/async/EventBase.h>

namespace facebook::ucachebench {

/**
 * Given an EventBase and callback function, it schedules the callback
 * function to run before each EventBase loop
 */
class UcacheBenchThreadLoopCallback : public folly::EventBase::LoopCallback {
 public:
  explicit UcacheBenchThreadLoopCallback(
      folly::EventBase* evb,
      std::function<void()> runBeforeLoop)
      : evb_(evb), runBeforeLoop_(std::move(runBeforeLoop)) {}

  void runLoopCallback() noexcept override {
    if (runBeforeLoop_) {
      runBeforeLoop_();
      evb_->runBeforeLoop(this);
    }
  }

  void cancelLoopCallback() {
    runBeforeLoop_ = nullptr;
  }

 private:
  folly::EventBase* evb_;
  std::function<void()> runBeforeLoop_;
};

} // namespace facebook::ucachebench

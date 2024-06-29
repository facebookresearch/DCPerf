// Copyright 2015 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef OLDISIM_LOGHISTOGRAMSAMPLER_H
#define OLDISIM_LOGHISTOGRAMSAMPLER_H

#include <assert.h>
#include <inttypes.h>
#include <math.h>

#include <limits>
#include <memory>
#include <vector>

#define _POW 1.1

namespace oldisim {

class LogHistogramSampler {
 public:
  std::vector<uint64_t> bins_;

  double sum_;
  double sum_sq_;

  LogHistogramSampler() = delete;
  explicit LogHistogramSampler(int _bins) : sum_(0), sum_sq_(0) {
    assert(_bins > 0);

    bins_.resize(_bins + 1, 0);
  }

  void sample(double s) {
    assert(s >= 0);
    size_t bin = log(s) / log(_POW);

    sum_ += s;
    sum_sq_ += s * s;

    if ((int64_t)bin < 0) {
      bin = 0;
    } else if (bin >= bins_.size()) {
      bin = bins_.size() - 1;
    }

    bins_[bin]++;
  }

  double average() const {
    if (total() == 0) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    return sum_ / total();
  }

  double stddev() const {
    if (total() == 0) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    return sqrt(sum_sq_ / total() - pow(sum_ / total(), 2.0));
  }

  double minimum() const {
    if (total() == 0) {
      return std::numeric_limits<double>::quiet_NaN();
    }
    for (size_t i = 0; i < bins_.size(); i++) {
      if (bins_.at(i) > 0) {
        return pow(_POW, static_cast<double>(i) + 0.5);
      }
    }
    DIE("Not implemented");
  }

  double get_nth(double nth) const {
    if (total() == 0) {
      return std::numeric_limits<double>::quiet_NaN();
    }

    uint64_t count = total();
    uint64_t n = 0;
    double target = count * nth / 100;

    for (size_t i = 0; i < bins_.size(); i++) {
      n += bins_.at(i);

      if (n > target) {  // The nth is inside bins_[i].
        double left = target - (n - bins_.at(i));
        return pow(_POW, static_cast<double>(i)) +
               left / bins_.at(i) *
                   (pow(_POW, static_cast<double>(i + 1)) -
                   pow(_POW, static_cast<double>(i)));
      }
    }

    return pow(_POW, bins_.size());
  }

  uint64_t total() const {
    uint64_t sum_ = 0.0;

    for (auto i : bins_) {
      sum_ += i;
    }

    return sum_;
  }

  void accumulate(const LogHistogramSampler& h) {
    assert(bins_.size() == h.bins_.size());

    for (size_t i = 0; i < bins_.size(); i++) {
      bins_[i] += h.bins_[i];
    }

    sum_ += h.sum_;
    sum_sq_ += h.sum_sq_;
  }

  void Reset() {
    for (auto& i : bins_) {
      i = 0;
    }
    sum_ = 0;
    sum_sq_ = 0;
  }
};
}  // namespace oldisim

#endif  // OLDISIM_LOGHISTOGRAMSAMPLER_H


// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/abr_bandwidth_estimator.h"

#include <algorithm>

namespace avbase::media {

AbrBandwidthEstimator::AbrBandwidthEstimator(Config config) : config_(config) {
  config_.new_sample_weight = std::clamp(config_.new_sample_weight, 0.01, 1.0);
}

void AbrBandwidthEstimator::OnSample(int64_t bytes, base::TimeDelta duration) {
  if (bytes <= 0 || duration <= base::TimeDelta()) {
    return;
  }
  // bits over seconds, guarded against a sub-millisecond sample producing
  // an absurd spike (a 1-byte probe over 0 would be infinity).
  const int64_t sample_bps =
      static_cast<int64_t>(bytes * 8 / std::max(duration.InSecondsF(), 0.001));
  ++samples_;
  if (samples_ == 1 || estimate_bps_ == 0) {
    estimate_bps_ = sample_bps;
    return;
  }
  const double weight = config_.new_sample_weight;
  estimate_bps_ = static_cast<int64_t>(estimate_bps_ * (1.0 - weight) +
                                       sample_bps * weight);
}

void AbrBandwidthEstimator::Reset() {
  samples_ = 0;
  estimate_bps_ = 0;
}

}  // namespace avbase::media

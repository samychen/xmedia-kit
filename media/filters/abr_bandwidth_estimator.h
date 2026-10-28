// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FILTERS_ABR_BANDWIDTH_ESTIMATOR_H_
#define AVBASE_MEDIA_FILTERS_ABR_BANDWIDTH_ESTIMATOR_H_

#include <stdint.h>

#include "base/time/time.h"
#include "media/media_export.h"

namespace avbase::media {

// Throughput estimate for ABR -- the port of CicadaPlayer's
// AbrRefererData/bandwidth tracking. Callers feed one sample per segment
// download (bytes / wall time); the estimate is an EWMA so a single slow
// segment cannot whipsaw the ladder, but a sustained drop converges within
// a handful of samples.
//
// Pure logic: no clock of its own, no threads -- the caller decides when a
// download "ended" and what duration to charge (CicadaPlayer charges the
// segment's download wall time; a test charges whatever it wants).
class AVBASE_MEDIA_EXPORT AbrBandwidthEstimator {
 public:
  struct Config {
    // Samples before bits_per_second() answers at all.
    int min_samples{3};
    // EWMA weight of a NEW sample (0..1]; higher = reacts faster.
    double new_sample_weight{0.3};
    // The first sample seeds the estimate directly.
  };

  AbrBandwidthEstimator() = default;
  explicit AbrBandwidthEstimator(Config config);

  void OnSample(int64_t bytes, base::TimeDelta duration);
  // 0 until |min_samples| samples have arrived.
  int64_t bits_per_second() const { return estimate_bps_; }
  int sample_count() const { return samples_; }
  void Reset();

 private:
  Config config_;
  int samples_ = 0;
  int64_t estimate_bps_ = 0;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_ABR_BANDWIDTH_ESTIMATOR_H_

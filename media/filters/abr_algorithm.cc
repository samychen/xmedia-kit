// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/abr_algorithm.h"

#include <algorithm>

namespace xmedia::media {

// The default argument cannot live in the header: a default argument is
// parsed before the enclosing class completes, so Config's NSDMIs are not
// usable there (the empty-config constructor delegates instead).
AbrBufferAlgoStrategy::AbrBufferAlgoStrategy()
    : AbrBufferAlgoStrategy(Config{}) {}

AbrBufferAlgoStrategy::AbrBufferAlgoStrategy(Config config) : config_(config) {}

AbrDecision AbrBufferAlgoStrategy::Decide(const AbrInput& input) {
  AbrDecision decision;
  const int rungs = static_cast<int>(input.variant_bandwidths_bps.size());
  if (rungs == 0) {
    return decision;
  }
  const int current = std::clamp(input.current_index, 0, rungs - 1);
  decision.target_index = current;

  // Panic: the pipeline is starving -- quality is the last concern.
  if (input.buffered_duration < config_.panic_buffer) {
    decision.target_index = 0;
    return decision;
  }

  // Drop when the estimate can no longer sustain the current rung.
  const int64_t current_bandwidth = input.variant_bandwidths_bps[current];
  if (input.estimated_bandwidth_bps > 0 &&
      input.estimated_bandwidth_bps < current_bandwidth) {
    // Land on the highest rung the estimate sustains.
    int target = 0;
    for (int i = 0; i < rungs; ++i) {
      if (input.estimated_bandwidth_bps >= input.variant_bandwidths_bps[i]) {
        target = i;
      }
    }
    decision.target_index = std::min(target, current);
    return decision;
  }

  // Climb only with both the buffer AND the bandwidth to afford it. The
  // ratio is applied in microseconds: TimeDelta has no double multiplier
  // (an implicit double->int64_t conversion would truncate 0.5 to 0 and
  // make every buffer look "healthy").
  const int64_t climb_mark_us = static_cast<int64_t>(
      static_cast<double>(input.buffer_capacity.InMicroseconds()) *
      config_.climb_buffer_ratio);
  const bool buffer_healthy =
      input.buffer_capacity > base::TimeDelta() &&
      input.buffered_duration.InMicroseconds() >= climb_mark_us;
  if (!buffer_healthy || current + 1 >= rungs) {
    return decision;
  }
  const int64_t next_bandwidth = input.variant_bandwidths_bps[current + 1];
  if (input.estimated_bandwidth_bps >=
      next_bandwidth * config_.climb_bandwidth_headroom) {
    decision.target_index = current + 1;
  }
  return decision;
}

}  // namespace xmedia::media

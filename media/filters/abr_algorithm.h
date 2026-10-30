// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FILTERS_ABR_ALGORITHM_H_
#define XMEDIA_MEDIA_FILTERS_ABR_ALGORITHM_H_

#include <stdint.h>

#include <vector>

#include "base/time/time.h"
#include "media/media_export.h"

namespace xmedia::media {

// The ABR decision interface -- the port of CicadaPlayer's
// AbrAlgoStrategy/AbrManager split: the demuxer feeds state, an algorithm
// answers "which rung of the ladder next", and the owner of the ladder
// performs the switch. Injectable so a product can replace the buffer
// heuristic without touching the demuxer (CicadaPlayer's same seam).
struct XMEDIA_MEDIA_EXPORT AbrInput {
  int64_t estimated_bandwidth_bps{0};
  int64_t current_variant_bandwidth_bps{0};
  // Ladder, ascending by bandwidth (the demuxer sorts once per playlist).
  std::vector<int64_t> variant_bandwidths_bps;
  int current_index{0};
  // Buffer state: how much is playable right now vs. the high-water mark
  // the player is willing to hold.
  base::TimeDelta buffered_duration;
  base::TimeDelta buffer_capacity;
};

struct XMEDIA_MEDIA_EXPORT AbrDecision {
  // The rung to be on for the NEXT segment. Equal to |current_index| means
  // stay. Decisions are advisory: the caller clamps and may ignore.
  int target_index{0};
};

class XMEDIA_MEDIA_EXPORT AbrAlgoStrategy {
 public:
  virtual ~AbrAlgoStrategy() = default;
  virtual AbrDecision Decide(const AbrInput& input) = 0;
};

// The default heuristic, aligned with CicadaPlayer's AbrBufferAlgoStrategy
// thinking: the buffer is the authority (a full buffer buys a higher rung,
// an empty one forces the floor), and bandwidth is the gate (never climb
// without 2x headroom over the next rung, drop immediately when the
// estimate cannot sustain the current rung).
class XMEDIA_MEDIA_EXPORT AbrBufferAlgoStrategy final : public AbrAlgoStrategy {
 public:
  struct Config {
    // Climb only when the buffer holds at least this fraction of capacity.
    double climb_buffer_ratio{0.5};
    // Bandwidth headroom required over the next rung (anti-flap hysteresis).
    double climb_bandwidth_headroom{2.0};
    // Panic floor: below this much buffer, take the cheapest rung.
    base::TimeDelta panic_buffer{base::Milliseconds(500)};
  };

  AbrBufferAlgoStrategy();
  explicit AbrBufferAlgoStrategy(Config config);
  AbrDecision Decide(const AbrInput& input) override;

 private:
  Config config_;
};

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_FILTERS_ABR_ALGORITHM_H_

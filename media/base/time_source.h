// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `media/base/time_source.h` and `wall_clock_time.h`
// (BSD-3-Clause).

#ifndef XMEDIA_MEDIA_BASE_TIME_SOURCE_H_
#define XMEDIA_MEDIA_BASE_TIME_SOURCE_H_

#include <vector>

#include "base/time/time.h"
#include "media/media_export.h"

namespace xmedia::media {

// A media timestamp paired with the wall-clock instant it should be visible.
// This is what lets a sink ask "which frame belongs in the refresh window that
// ends at T?" instead of guessing (see VideoFrameCompositor::Render).
struct XMEDIA_MEDIA_EXPORT WallClockTime {
  base::TimeDelta media_time;
  base::TimeTicks wall_time;
};

class XMEDIA_MEDIA_EXPORT TimeSource {
 public:
  TimeSource(const TimeSource&) = delete;
  TimeSource& operator=(const TimeSource&) = delete;
  virtual ~TimeSource() = default;

  // Maps each entry of |media_timestamps| to a wall-clock instant, using
  // |reference_time| as "now" for the media clock. Thread-safe.
  virtual void
  GetWallClockTimes(const std::vector<base::TimeDelta>& media_timestamps,
                    base::TimeTicks reference_time,
                    std::vector<WallClockTime>* wall_clock_times) = 0;

 protected:
  TimeSource() = default;
};

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_BASE_TIME_SOURCE_H_

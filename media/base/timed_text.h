// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_BASE_TIMED_TEXT_H_
#define XMEDIA_MEDIA_BASE_TIMED_TEXT_H_

#include <string>

#include "base/time/time.h"
#include "media/media_export.h"

namespace xmedia::media {

// One decoded subtitle cue. The base's contract for text tracks is "text +
// time only" (xmedia-kit upgrade plan §Phase 4.2): layout and rendering belong
// to
// the host, which is why both a plain string and the raw markup are carried.
struct XMEDIA_MEDIA_EXPORT TimedTextCue {
  // Which source this cue belongs to (docs/14 stage 4). The host's subtitle
  // overlay keys its surface on this, so a secondary source's track renders on
  // its own layer instead of over the primary. 0 for single-source and for any
  // pre-stage-4 consumer that never set it.
  int source_id = 0;
  base::TimeDelta pts;
  base::TimeDelta duration;
  std::string text;  // Plain text, all cue lines joined with '\n'.
  std::string ass;   // Raw ASS markup (ASS-type rects), empty for plain.
};

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_BASE_TIMED_TEXT_H_

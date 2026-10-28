// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/external_text_track.h"

#include <algorithm>

namespace avbase::media {

base::expected<ExternalTextTrack, MediaError>
ExternalTextTrack::Parse(std::string_view text, const std::string& uri,
                         int track_id) {
  std::string_view body = text;
  // BOM tolerance before the WEBVTT signature check.
  if (body.size() >= 3 && body.starts_with("\xEF\xBB\xBF")) {
    body.remove_prefix(3);
  }
  std::vector<TimedTextCue> cues;
  if (body.starts_with("WEBVTT")) {
    auto parsed = ParseWebVtt(body, track_id);
    if (!parsed.has_value()) {
      return base::unexpected(parsed.error());
    }
    cues = std::move(*parsed);
  } else {
    auto parsed = ParseSrt(body, track_id);
    if (!parsed.has_value()) {
      return base::unexpected(parsed.error());
    }
    cues = std::move(*parsed);
  }
  std::sort(cues.begin(), cues.end(),
            [](const TimedTextCue& a, const TimedTextCue& b) {
              return a.pts < b.pts;
            });
  ExternalTextTrack track;
  track.track_id_ = track_id;
  track.uri_ = uri;
  track.cues_ = std::move(cues);
  return track;
}

void ExternalTextTrack::SetDelay(base::TimeDelta delay) {
  delay_ = delay;
}

std::vector<TimedTextCue>
ExternalTextTrack::CuesBetween(base::TimeDelta prev,
                               base::TimeDelta now) const {
  if (!selected_ || now <= prev) {
    return {};
  }
  std::vector<TimedTextCue> due;
  for (const TimedTextCue& cue : cues_) {
    // The cue is shifted by the delay; it covers [pts, pts+duration). It is
    // "on" somewhere inside the (prev, now] window when it overlaps it, and
    // it is reported once per window it overlaps.
    const base::TimeDelta start = cue.pts + delay_;
    const base::TimeDelta end = start + cue.duration;
    if (end > prev && start <= now) {
      due.push_back(cue);
    }
  }
  return due;
}

}  // namespace avbase::media

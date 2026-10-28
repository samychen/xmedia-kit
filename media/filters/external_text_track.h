// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FILTERS_EXTERNAL_TEXT_TRACK_H_
#define AVBASE_MEDIA_FILTERS_EXTERNAL_TEXT_TRACK_H_

#include <string>
#include <vector>

#include "base/time/time.h"
#include "media/base/timed_text.h"
#include "media/filters/srt_parser.h"
#include "media/filters/webvtt_parser.h"
#include "media/media_export.h"

namespace avbase::media {

// One external subtitle track -- the scheduling half of CicadaPlayer's
// addExtSubtitle/selectExtSubtitle/setStreamDelay trio. The parser half
// lives in srt_parser/webvtt_parser; this class holds the parsed cues, the
// track's delay (CicadaPlayer's setStreamDelayTime: negative moves the
// track earlier), and answers which cues are on screen between two clock
// reads. Pure and single-threaded: the owner serialises access.
class AVBASE_MEDIA_EXPORT ExternalTextTrack {
 public:
  // Parses |text| as SRT or WebVTT by shape: a WEBVTT signature wins,
  // otherwise the SRT grammar decides (a file that parses as neither is an
  // error with an actionable message).
  static base::expected<ExternalTextTrack, MediaError>
  Parse(std::string_view text, const std::string& uri, int track_id);

  // Shifts every cue by |delay| (CicadaPlayer's setStreamDelayTime).
  void SetDelay(base::TimeDelta delay);
  base::TimeDelta delay() const { return delay_; }

  int track_id() const { return track_id_; }
  const std::string& uri() const { return uri_; }
  size_t cue_count() const { return cues_.size(); }
  bool selected() const { return selected_; }
  void SetSelected(bool selected) { selected_ = selected; }

  // Cues whose on-screen interval covers any moment in (prev, now] -- the
  // show set for a clock that advanced from |prev| to |now|. Empty when the
  // track is not selected. A cue appears once per interval it covers.
  std::vector<TimedTextCue> CuesBetween(base::TimeDelta prev,
                                        base::TimeDelta now) const;

 private:
  ExternalTextTrack() = default;
  int track_id_ = 0;
  std::string uri_;
  bool selected_ = true;
  base::TimeDelta delay_;
  std::vector<TimedTextCue> cues_;  // sorted by pts
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_EXTERNAL_TEXT_TRACK_H_

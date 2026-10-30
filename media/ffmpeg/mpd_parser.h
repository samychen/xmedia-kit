// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FFMPEG_MPD_PARSER_H_
#define XMEDIA_MEDIA_FFMPEG_MPD_PARSER_H_

#include <stdint.h>

#include <string>
#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "media/base/media_error.h"
#include "media/media_export.h"

namespace xmedia::media::dash {

// The DASH baseline (cicada-port T8): a minimal MPD reader standing in for
// CicadaPlayer's dash/MPDParser, scoped to what a SegmentTemplate follower
// consumes. The full DASH demuxer integration (multiple Periods,
// SegmentList/SegmentBase, UTCTiming clock sync, the demuxer wiring) is the
// documented follow-up; this file is deliberately complete and tested on
// its own so that integration starts from solid ground.

struct XMEDIA_MEDIA_EXPORT MpdRepresentation {
  std::string id;
  int64_t bandwidth{0};   // bits per second, as advertised
  std::string mime_type;  // "video/mp4", "audio/mp4", ...
  std::string codecs;     // RFC 6381, may be empty
  // Resolved per-segment URLs for this representation, in play order
  // (initialisation segment first when SegmentTemplate names one).
  std::vector<std::string> segment_urls;
  base::TimeDelta segment_duration;  // per-segment duration
  base::TimeDelta total_duration;    // sum over the segments
};

struct XMEDIA_MEDIA_EXPORT Mpd {
  bool is_live{false};  // dynamic profile: no fixed duration, no EOF
  base::TimeDelta media_presentation_duration;
  std::string type;  // "static" / "dynamic", verbatim
  // The video-typed representations (audio-only MPDs list audio here too;
  // the caller picks by mime_type). Ordered as the manifest listed them.
  std::vector<MpdRepresentation> representations;
};

// Parses an MPD body. Supported: MPD@type/mediaPresentationDuration,
// BaseURL (flat), one Period, AdaptationSet@contentType/mimeType and
// Representation[id/bandwidth/mimeType/codecs], SegmentTemplate@media/
// initialization/startNumber/duration/timescale with $Number$ and
// $RepresentationID$ templates. Everything else is ignored, not an error
// (the same posture the m3u8 parser takes).
XMEDIA_MEDIA_EXPORT base::expected<Mpd, MediaError>
ParseMpd(std::string_view text, const std::string& base_url);

// Resolves relative URIs the way the m3u8 parser does (same four shapes).
XMEDIA_MEDIA_EXPORT std::string ResolveMpdUrl(const std::string& base_url,
                                              std::string_view relative);

}  // namespace xmedia::media::dash

#endif  // XMEDIA_MEDIA_FFMPEG_MPD_PARSER_H_

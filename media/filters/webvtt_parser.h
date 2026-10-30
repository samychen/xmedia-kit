// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FILTERS_WEBVTT_PARSER_H_
#define XMEDIA_MEDIA_FILTERS_WEBVTT_PARSER_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "media/base/media_error.h"
#include "media/base/timed_text.h"
#include "media/media_export.h"

namespace xmedia::media {

// WebVTT (.vtt) -> TimedTextCue, the external-subtitle counterpart of the
// embedded-VTT leg. Handles the WEBVTT signature (with optional BOM),
// "hh:mm:ss.mmm --> hh:mm:ss.mmm" cue timings (with and without hours),
// cue identifiers and NOTE/STYLE/REGION blocks (skipped), and cue payload
// lines joined with '\n'. Inline markup tags are stripped, like the SRT
// parser, so the cue text matches what the embedded text leg delivers.
XMEDIA_MEDIA_EXPORT base::expected<std::vector<TimedTextCue>, MediaError>
ParseWebVtt(std::string_view text, int source_id);

// "mm:ss.mmm" / "hh:mm:ss.mmm" -> TimeDelta.
XMEDIA_MEDIA_EXPORT base::expected<base::TimeDelta, MediaError>
ParseVttTimestamp(std::string_view stamp);

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_FILTERS_WEBVTT_PARSER_H_

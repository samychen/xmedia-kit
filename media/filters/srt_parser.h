// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FILTERS_SRT_PARSER_H_
#define XMEDIA_MEDIA_FILTERS_SRT_PARSER_H_

#include <string>
#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "media/base/media_error.h"
#include "media/base/timed_text.h"
#include "media/media_export.h"

namespace xmedia::media {

// SubRip (.srt) -> TimedTextCue -- the parser behind CicadaPlayer's
// addExtSubtitle for SRT files. Blocks are "index / 00:00:01,000 -->
// 00:00:02,000 / text lines", separated by blank lines. The index line is
// tolerated-absent (real files sometimes skip it); HTML-ish tags (<i>, <b>,
// <font ...>) are stripped, matching what the text leg delivers for
// embedded SRT.
XMEDIA_MEDIA_EXPORT base::expected<std::vector<TimedTextCue>, MediaError>
ParseSrt(std::string_view text, int source_id);

// "00:00:01,000" / "00:00:01.000" -> TimeDelta.
XMEDIA_MEDIA_EXPORT base::expected<base::TimeDelta, MediaError>
ParseSrtTimestamp(std::string_view stamp);

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_FILTERS_SRT_PARSER_H_

// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Lives in media/base: media::Demuxer produces it. player/public/media_info.h
// re-exports it (invariant C22).

#ifndef AVBASE_MEDIA_BASE_MEDIA_INFO_H_
#define AVBASE_MEDIA_BASE_MEDIA_INFO_H_

#include <stdint.h>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/time/time.h"
#include "media/base/media_types.h"
#include "media/base/video_frame.h"
#include "media/media_export.h"

namespace avbase::media {

enum class StreamKind { kUnknown, kAudio, kVideo, kText };

struct AVBASE_MEDIA_EXPORT StreamInfo {
  int index{-1};
  StreamKind kind{StreamKind::kUnknown};
  std::string codec_name;  // "h264", "hevc", "aac", "opus", "subrip"...
  std::string language;    // ISO-639-2, empty when unknown.
  std::string title;
  base::TimeDelta duration;

  // Video only.
  media::Size coded_size;
  media::Size natural_size;  // After SAR is applied.
  media::Rational sar{1, 1};
  media::Rational frame_rate{0, 1};
  media::Rational avg_frame_rate{0, 1};
  int rotation{0};
  std::string pixel_format;
  // True when the video stream's transfer characteristics are PQ (ST 2084) or
  // HLG (ARIB STD-B67), or the stream carries Dolby Vision / HDR10+ dynamic
  // metadata. The renderer has no tone mapping (docs/TODO §1), so a host that
  // cares can warn, tint, or refuse -- but it needs to be TOLD, which is what
  // kHdrUnsupported is for.
  bool has_hdr_metadata{false};
  // FFmpeg's name for the transfer characteristic ("smpte2084", "arib-std-b67",
  // "bt709", ...). Carried next to the flag because "HDR" is not one thing:
  // PQ and HLG need different treatment, and a host showing a warning wants to
  // name the actual curve rather than say "HDR".
  std::string color_transfer;

  // Audio only.
  int sample_rate{0};
  int channels{0};
  std::string channel_layout;
  std::string sample_format;

  int64_t bit_rate{0};
  bool is_default{false};
  bool is_forced{false};
  std::map<std::string, std::string> metadata;
};

// Everything the container told us, as one immutable snapshot. Replaces
// ijkplayer's ijkmeta.c plus a dozen ijkmp_get_* accessors.
struct AVBASE_MEDIA_EXPORT MediaInfo {
  base::TimeDelta duration;
  // Live streams report a zero duration; use |is_live| rather than testing it.
  bool is_live{false};
  bool seekable{true};
  int64_t bit_rate{0};
  int64_t file_size{-1};
  std::string format_name;  // "mov,mp4,m4a,3gp,3g2,mj2"
  std::string uri;
  base::TimeDelta start_time;
  std::vector<StreamInfo> streams;
  std::map<std::string, std::string> metadata;

  // -1 when there is no such stream.
  int FirstStreamOfKind(StreamKind kind) const;
  const StreamInfo* FindStream(int index) const;
  std::vector<const StreamInfo*> StreamsOfKind(StreamKind kind) const;
  // Total duration may be an estimate for streaming containers.
  bool duration_is_estimate{false};
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_BASE_MEDIA_INFO_H_

// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FFMPEG_HLS_M3U8_PARSER_H_
#define XMEDIA_MEDIA_FFMPEG_HLS_M3U8_PARSER_H_

#include <stdint.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/time/time.h"
#include "media/base/media_error.h"
#include "media/media_export.h"

namespace xmedia::media::hls {

// The HLS playlist model, ported from CicadaPlayer's
// framework/demuxer/play_list (HlsParser/HlsTags/PlaylistManager) down to
// what an xmedia-kit demuxer consumes: master playlists (variant streams to
// switch between), media playlists (the segment timeline to serve), and the
// tags that change how a segment is fetched or decrypted.
//
// PARSED-NOT-IMPLEMENTED. LL-HLS tags (EXT-X-PART / EXT-X-PART-INF /
// EXT-X-PRELOAD-HINT) are recognised and recorded (has_ll_hls_tags) so a
// caller can detect a low-latency stream, but part prefetching is not built
// yet -- the playlist demuxer treats such a stream as an ordinary live
// playlist. EXT-X-DATERANGE and EXT-X-I-FRAMES-ONLY are CicadaPlayer's
// documented unsupported list too. EXT-X-PROGRAM-DATE-TIME IS parsed
// (Segment::program_date_time_ms) and feeds the player's UTC anchor.

struct XMEDIA_MEDIA_EXPORT KeyInfo {
  // EXT-X-KEY:METHOD -- NONE, AES-128, SAMPLE-AES.
  std::string method{"NONE"};
  // Applied per segment: the key active when the segment tag appeared.
  std::string uri;
  std::string iv_hex;
  // NONE clears everything above.
  bool is_none() const { return method == "NONE"; }
  bool is_aes_128() const { return method == "AES-128"; }
};

struct XMEDIA_MEDIA_EXPORT Segment {
  std::string uri;  // resolved against the playlist URL
  base::TimeDelta duration;
  std::string title;  // EXTINF's comma tail
  // EXT-X-DISCONTINUITY directly before this segment: the decoder state
  // resets here (timestamps and/or codec parameters may jump).
  bool discontinuity{false};
  KeyInfo key;  // active key for this segment (CicadaPlayer: per-segment)
  // EXT-X-BYTERANGE: a byte sub-range of the URI. length -1 with a start
  // >= 0 means "same length as the previous segment" (spec rule).
  int64_t byterange_start{-1};
  int64_t byterange_length{-1};
  // Resolved absolute sequence number (playlist media_sequence + index).
  int64_t media_sequence{0};
  // The playlist-timeline position this segment starts at: the running sum
  // of the durations of every segment before it. This is what the demuxer's
  // seek maps timestamps with and what segment switches align on.
  base::TimeDelta timeline_start;
  // EXT-X-PROGRAM-DATE-TIME of this segment's first sample, as Unix epoch
  // milliseconds (UTC). The tag applies to the segment that follows it;
  // segments without the tag inherit the mapping implicitly, which the
  // DEMUXER extrapolates from the anchor (the last tagged segment).
  bool has_program_date_time{false};
  int64_t program_date_time_ms{0};
};

struct XMEDIA_MEDIA_EXPORT Variant {
  // EXT-X-STREAM-INF attributes (ABR candidates).
  int bandwidth{0};
  int average_bandwidth{0};
  std::string codecs;  // comma-separated RFC 6381
  std::string resolution;
  std::string name;  // NAME attribute, may be empty
  std::string uri;   // resolved against the MASTER playlist URL
  // Not a spec attribute: the demuxer's ABR ladder tags each Variant with
  // its position in the master's list so a switch can name the URI.
  int variant_index{0};
};

struct XMEDIA_MEDIA_EXPORT Rendition {
  // EXT-X-MEDIA (audio/subtitle renditions of a GROUP-ID).
  std::string type;  // AUDIO / SUBTITLES
  std::string group_id;
  std::string name;
  std::string language;
  std::string uri;  // may be empty for a rendition with no own media
  bool is_default{false};
  bool is_autoselect{false};
};

struct XMEDIA_MEDIA_EXPORT Playlist {
  bool is_master{false};
  bool is_live{false};  // no EXT-X-ENDLIST
  int target_duration{0};
  int64_t media_sequence{0};
  int discontinuity_sequence{0};
  base::TimeDelta total_duration;  // sum of EXTINF (vod) / window (live)
  bool has_ll_hls_tags{false};
  bool has_endlist{false};
  std::string version;
  std::vector<Segment> segments;      // media playlist
  std::vector<Variant> variants;      // master playlist
  std::vector<Rendition> renditions;  // master playlist
  KeyInfo session_key;                // last EXT-X-KEY seen (media)
};

// Parses an M3U8 body. |base_url| resolves the playlist's relative URIs
// against (segments and variant URIs come back absolute). Fails with a
// structured error when the body is not an M3U8 (#EXTM3U missing) or a
// required attribute is malformed -- a playlist that parses to zero
// segments AND zero variants is also an error: there is nothing to play.
XMEDIA_MEDIA_EXPORT base::expected<Playlist, MediaError>
ParseM3U8(std::string_view text, const std::string& base_url);

// Resolves |relative| against |base_url| the way an HLS client must:
// absolute URIs pass through, scheme-relative inherit the scheme,
// root-relative inherit the authority, everything else joins the base
// directory. Deliberately not a full RFC 3986 resolver -- HLS URIs in the
// wild are one of those four shapes.
XMEDIA_MEDIA_EXPORT std::string ResolveUrl(const std::string& base_url,
                                           std::string_view relative);

// Parses an ISO 8601 datetime ("2026-10-31T12:00:00.500Z", "…+08:00",
// "…-05:30"; the "T" may be a space) into Unix epoch MILLISECONDS. A
// datetime without a zone counts as UTC (the HLS spec's own examples are
// all Z-suffixed). Returns nullopt on anything it cannot fully parse --
// callers treat that as "no anchor", never as an error.
XMEDIA_MEDIA_EXPORT std::optional<int64_t>
ParseIsoDateTimeMs(std::string_view text);

}  // namespace xmedia::media::hls

#endif  // XMEDIA_MEDIA_FFMPEG_HLS_M3U8_PARSER_H_

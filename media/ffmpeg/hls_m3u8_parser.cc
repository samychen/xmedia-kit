// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/hls_m3u8_parser.h"

#include <cctype>
#include <cstdlib>
#include <map>
#include <optional>

namespace avbase::media::hls {
namespace {

// Splits an attribute list ("BANDWIDTH=1280,CODECS=\"a,b\",NAME=x") into
// name -> value. Quoted values may contain commas and '='; quotes are
// stripped. Unknown-value shapes become empty entries the caller ignores.
std::map<std::string, std::string> ParseAttributeList(std::string_view text) {
  std::map<std::string, std::string> attrs;
  size_t cursor = 0;
  while (cursor < text.size()) {
    const size_t eq = text.find('=', cursor);
    if (eq == std::string_view::npos) {
      break;
    }
    std::string name(text.substr(cursor, eq - cursor));
    for (char& c : name) {
      c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    size_t value_end = eq + 1;
    if (value_end < text.size() && text[value_end] == '"') {
      const size_t close = text.find('"', value_end + 1);
      if (close == std::string_view::npos) {
        break;  // unterminated quote: drop the rest
      }
      attrs[name] =
          std::string(text.substr(value_end + 1, close - value_end - 1));
      value_end = close + 1;
    } else {
      const size_t comma = text.find(',', value_end);
      value_end = comma == std::string_view::npos ? text.size() : comma;
      attrs[name] = std::string(text.substr(eq + 1, value_end - eq - 1));
    }
    const size_t next = text.find(',', value_end);
    cursor = next == std::string_view::npos ? text.size() : next + 1;
  }
  return attrs;
}

std::optional<int64_t> ParseInt(std::string_view value) {
  if (value.empty()) {
    return std::nullopt;
  }
  int64_t parsed = 0;
  for (char c : value) {
    if (c < '0' || c > '9') {
      return std::nullopt;
    }
    parsed = parsed * 10 + (c - '0');
  }
  return parsed;
}

// "WxH" -> the pair; std::nullopt on anything else.
std::optional<std::pair<int64_t, int64_t>>
ParseResolution(std::string_view value) {
  const size_t x = value.find('x');
  if (x == std::string_view::npos || x == 0 || x + 1 == value.size()) {
    return std::nullopt;
  }
  auto width = ParseInt(value.substr(0, x));
  auto height = ParseInt(value.substr(x + 1));
  if (!width || !height) {
    return std::nullopt;
  }
  return std::make_pair(*width, *height);
}

}  // namespace

std::string ResolveUrl(const std::string& base_url, std::string_view relative) {
  if (relative.empty()) {
    return base_url;
  }
  if (relative.starts_with("http://") || relative.starts_with("https://") ||
      relative.starts_with("file://") || relative.starts_with("data:")) {
    return std::string(relative);
  }
  const size_t scheme_end = base_url.find("://");
  if (scheme_end == std::string::npos) {
    // Local-path playlists: join like a filesystem when the base is a path.
    if (relative.starts_with("/")) {
      return std::string(relative);
    }
    const size_t slash = base_url.rfind('/');
    if (slash == std::string::npos) {
      return std::string(relative);
    }
    return base_url.substr(0, slash + 1) + std::string(relative);
  }
  const size_t authority_start = scheme_end + 3;
  const size_t path_start = base_url.find('/', authority_start);
  if (relative.starts_with("//")) {
    // Scheme-relative: inherit the scheme.
    return base_url.substr(0, scheme_end + 1) + std::string(relative);
  }
  if (relative.starts_with("/")) {
    // Root-relative: inherit scheme + authority.
    if (path_start == std::string::npos) {
      return base_url + std::string(relative);
    }
    return base_url.substr(0, path_start) + std::string(relative);
  }
  // Directory-relative.
  const std::string dir = path_start == std::string::npos
                              ? base_url + "/"
                              : base_url.substr(0, base_url.rfind('/') + 1);
  return dir + std::string(relative);
}

base::expected<Playlist, MediaError> ParseM3U8(std::string_view text,
                                               const std::string& base_url) {
  Playlist playlist;
  // Running state that tags apply to the NEXT segment.
  base::TimeDelta timeline = base::TimeDelta();
  KeyInfo active_key;
  bool pending_discontinuity = false;
  bool saw_extinf = false;
  base::TimeDelta extinf_duration;
  std::string extinf_title;
  bool saw_stream_inf = false;
  Variant pending_variant;
  std::optional<int64_t> pending_byterange_length;
  std::optional<int64_t> pending_byterange_start;
  int64_t sequence_counter = playlist.media_sequence;

  size_t line_start = 0;
  bool first_line_checked = false;
  while (line_start <= text.size()) {
    size_t line_end = text.find('\n', line_start);
    if (line_end == std::string_view::npos) {
      line_end = text.size();
    }
    std::string_view line = text.substr(line_start, line_end - line_start);
    line_start = line_end + 1;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
      line.remove_suffix(1);
    }
    if (line.empty()) {
      continue;
    }
    if (!first_line_checked) {
      first_line_checked = true;
      // BOM tolerance, then the #EXTM3U signature the spec requires.
      if (line.starts_with("\xEF\xBB\xBF")) {
        line.remove_prefix(3);
      }
      if (!line.starts_with("#EXTM3U")) {
        return base::unexpected(MediaError::Of(
            ErrorCode::kSourceUnsupported, "not an m3u8 playlist",
            "first line = " + std::string(line.substr(0, 32)),
            "check the url content-type, or the server's error body"));
      }
      continue;
    }

    if (line.starts_with("#EXT")) {
      const size_t colon = line.find(':');
      const std::string tag(line.substr(
          0, colon == std::string_view::npos ? line.size() : colon));
      const std::string_view value = colon == std::string_view::npos
                                         ? std::string_view()
                                         : line.substr(colon + 1);
      if (tag == "#EXTM3U") {
        continue;
      } else if (tag == "#EXT-X-VERSION") {
        playlist.version = std::string(value);
      } else if (tag == "#EXT-X-TARGETDURATION") {
        playlist.target_duration =
            static_cast<int>(ParseInt(value).value_or(0));
      } else if (tag == "#EXT-X-MEDIA-SEQUENCE") {
        playlist.media_sequence = ParseInt(value).value_or(0);
        sequence_counter = playlist.media_sequence;
      } else if (tag == "#EXT-X-DISCONTINUITY-SEQUENCE") {
        playlist.discontinuity_sequence =
            static_cast<int>(ParseInt(value).value_or(0));
      } else if (tag == "#EXT-X-ENDLIST") {
        playlist.has_endlist = true;
      } else if (tag == "#EXTINF") {
        // "<duration>[,<title>]"
        const size_t comma = value.find(',');
        const std::string_view dur =
            comma == std::string_view::npos ? value : value.substr(0, comma);
        // EXTINF carries fractional seconds; TimeDelta's Seconds() takes a
        // whole count, so resolve to microseconds (EXTINF precision is
        // millisecond-scale at most in every playlist this client meets).
        const double seconds = std::strtod(std::string(dur).c_str(), nullptr);
        extinf_duration =
            base::Microseconds(static_cast<int64_t>(seconds * 1000000.0 + 0.5));
        extinf_title = comma == std::string_view::npos
                           ? std::string()
                           : std::string(value.substr(comma + 1));
        saw_extinf = true;
      } else if (tag == "#EXT-X-KEY") {
        auto attrs = ParseAttributeList(value);
        KeyInfo key;
        key.method = attrs.count("METHOD") ? attrs["METHOD"] : "NONE";
        if (attrs.count("URI")) {
          key.uri = ResolveUrl(base_url, attrs["URI"]);
        }
        if (attrs.count("IV")) {
          key.iv_hex = attrs["IV"];
        }
        if (key.is_none()) {
          active_key = KeyInfo();  // NONE clears the key for later segments
        } else {
          active_key = key;
          playlist.session_key = key;
        }
      } else if (tag == "#EXT-X-DISCONTINUITY") {
        pending_discontinuity = true;
      } else if (tag == "#EXT-X-BYTERANGE") {
        // "<length>[@<start>]", placed between EXTINF and the URI line: it
        // applies to the NEXT segment. A start without an explicit value
        // continues where the previous segment's range ended (spec rule);
        // both are resolved after the URI arrives.
        const size_t at = value.find('@');
        pending_byterange_length = ParseInt(
            at == std::string_view::npos ? value : value.substr(0, at));
        pending_byterange_start = std::nullopt;
        if (at != std::string_view::npos) {
          pending_byterange_start = ParseInt(value.substr(at + 1));
        }
      } else if (tag == "#EXT-X-PART" || tag == "#EXT-X-PART-INF" ||
                 tag == "#EXT-X-PRELOAD-HINT") {
        playlist.has_ll_hls_tags = true;
      } else if (tag == "#EXT-X-STREAM-INF") {
        auto attrs = ParseAttributeList(value);
        pending_variant = Variant();
        pending_variant.bandwidth =
            static_cast<int>(ParseInt(attrs["BANDWIDTH"]).value_or(0));
        pending_variant.average_bandwidth =
            static_cast<int>(ParseInt(attrs["AVERAGE-BANDWIDTH"]).value_or(0));
        pending_variant.codecs = attrs["CODECS"];
        pending_variant.name = attrs["NAME"];
        if (auto resolution = ParseResolution(attrs["RESOLUTION"])) {
          pending_variant.resolution = std::to_string(resolution->first) + "x" +
                                       std::to_string(resolution->second);
        }
        saw_stream_inf = true;
      } else if (tag == "#EXT-X-MEDIA") {
        auto attrs = ParseAttributeList(value);
        Rendition rendition;
        rendition.type = attrs["TYPE"];
        rendition.group_id = attrs["GROUP-ID"];
        rendition.name = attrs["NAME"];
        rendition.language = attrs["LANGUAGE"];
        if (attrs.count("URI")) {
          rendition.uri = ResolveUrl(base_url, attrs["URI"]);
        }
        rendition.is_default = attrs["DEFAULT"] == "YES";
        rendition.is_autoselect = attrs["AUTOSELECT"] == "YES";
        playlist.renditions.push_back(rendition);
      }
      // Unrecognised #EXT-X-* tags: ignored per spec (unknown tags must not
      // fail the playlist unless they carry the Critical marker, which none
      // of the ones this client ships with do).
      continue;
    }

    // A URI line.
    if (line.starts_with('#')) {
      continue;  // comment
    }
    if (saw_stream_inf) {
      Variant variant = pending_variant;
      variant.uri = ResolveUrl(base_url, line);
      playlist.variants.push_back(variant);
      playlist.is_master = true;
      saw_stream_inf = false;
      continue;
    }
    if (saw_extinf) {
      Segment segment;
      segment.uri = ResolveUrl(base_url, line);
      segment.duration = extinf_duration;
      segment.title = extinf_title;
      segment.discontinuity = pending_discontinuity;
      pending_discontinuity = false;
      segment.key = active_key;
      segment.media_sequence = sequence_counter++;
      segment.timeline_start = timeline;
      timeline += segment.duration;
      if (pending_byterange_length) {
        segment.byterange_length = *pending_byterange_length;
        if (pending_byterange_start) {
          segment.byterange_start = *pending_byterange_start;
        } else if (!playlist.segments.empty() &&
                   playlist.segments.back().byterange_length >= 0) {
          // No explicit start: continue after the previous range.
          const Segment& previous = playlist.segments.back();
          segment.byterange_start =
              previous.byterange_start + previous.byterange_length;
        }
        pending_byterange_length = std::nullopt;
        pending_byterange_start = std::nullopt;
      }
      playlist.segments.push_back(segment);
      saw_extinf = false;
      continue;
    }
    // A bare URI with no EXTINF: spec allows it (implicit duration), but
    // without a duration the timeline cannot be built -- record it with a
    // zero duration so the caller can at least see the shape.
    Segment segment;
    segment.uri = ResolveUrl(base_url, line);
    segment.key = active_key;
    segment.media_sequence = sequence_counter++;
    segment.timeline_start = timeline;
    playlist.segments.push_back(segment);
  }

  playlist.is_live = !playlist.has_endlist;
  playlist.total_duration = timeline;
  if (playlist.segments.empty() && playlist.variants.empty()) {
    return base::unexpected(MediaError::Of(
        ErrorCode::kSourceUnsupported, "the playlist has no media",
        "segments = 0, variants = 0",
        "check that the url is a media or master playlist, not an error "
        "page"));
  }
  return playlist;
}

}  // namespace avbase::media::hls

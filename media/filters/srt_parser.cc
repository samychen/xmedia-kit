// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/srt_parser.h"

#include <cstdio>

namespace avbase::media {
namespace {

// Strips the <tag> shapes real SRT files carry (<i>, </b>, <font ...>).
// Cue text goes to the renderer as plain text (TimedTextCue::text).
std::string StripTags(std::string_view line) {
  std::string out;
  bool in_tag = false;
  for (char c : line) {
    if (c == '<') {
      in_tag = true;
    } else if (c == '>') {
      in_tag = false;
    } else if (!in_tag) {
      out.push_back(c);
    }
  }
  return out;
}

}  // namespace

base::expected<base::TimeDelta, MediaError>
ParseSrtTimestamp(std::string_view stamp) {
  // hh:mm:ss,mmm (or with a '.'); every component must be present.
  int hours = 0;
  int minutes = 0;
  int seconds = 0;
  int millis = 0;
  if (std::sscanf(std::string(stamp).c_str(), "%d:%d:%d%*1[,.]%d", &hours,
                  &minutes, &seconds, &millis) != 4) {
    return base::unexpected(MediaError::Of(
        ErrorCode::kSourceUnsupported, "bad srt timestamp",
        "stamp = " + std::string(stamp),
        "expected hh:mm:ss,mmm -- check that the file is really SubRip"));
  }
  return base::Milliseconds(hours * 3600000 + minutes * 60000 + seconds * 1000 +
                            millis);
}

base::expected<std::vector<TimedTextCue>, MediaError>
ParseSrt(std::string_view text, int source_id) {
  std::vector<TimedTextCue> cues;
  std::vector<std::string_view> lines;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view line = text.substr(start, end - start);
    start = end + 1;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    lines.push_back(line);
  }

  base::TimeDelta cue_start;
  base::TimeDelta cue_end;
  std::string cue_text;
  bool in_cue = false;
  for (const std::string_view& line : lines) {
    if (!in_cue) {
      // A timestamp line opens a cue (the index line before it is optional).
      auto arrow = line.find("-->");
      if (arrow == std::string_view::npos) {
        continue;  // index line, blank line, or junk before the first cue
      }
      auto parsed_start = ParseSrtTimestamp(line.substr(0, arrow));
      if (!parsed_start.has_value()) {
        return base::unexpected(parsed_start.error());
      }
      std::string_view tail = line.substr(arrow + 3);
      const size_t tail_start = tail.find_first_not_of(" \t");
      if (tail_start == std::string_view::npos) {
        return base::unexpected(MediaError::Of(
            ErrorCode::kSourceUnsupported, "srt cue has no end timestamp",
            "line = " + std::string(line),
            "expected 'start --> end' on the timestamp line"));
      }
      auto parsed_end = ParseSrtTimestamp(tail.substr(tail_start));
      if (!parsed_end.has_value()) {
        return base::unexpected(parsed_end.error());
      }
      cue_start = *parsed_start;
      cue_end = *parsed_end;
      cue_text.clear();
      in_cue = true;
      continue;
    }
    if (line.empty()) {
      // Blank line: the cue is over. Overlapping/malformed ranges still
      // produce the cue -- the renderer's clock decides what shows.
      if (!cue_text.empty()) {
        TimedTextCue cue;
        cue.source_id = source_id;
        cue.pts = cue_start;
        cue.duration = cue_end - cue_start;
        cue.text = cue_text;
        cues.push_back(std::move(cue));
      }
      in_cue = false;
      continue;
    }
    if (!cue_text.empty()) {
      cue_text.push_back('\n');
    }
    cue_text += StripTags(line);
  }
  if (in_cue && !cue_text.empty()) {
    TimedTextCue cue;
    cue.source_id = source_id;
    cue.pts = cue_start;
    cue.duration = cue_end - cue_start;
    cue.text = cue_text;
    cues.push_back(std::move(cue));
  }
  return cues;
}

}  // namespace avbase::media

// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/webvtt_parser.h"

#include <cstdio>

namespace avbase::media {
namespace {

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
ParseVttTimestamp(std::string_view stamp) {
  int hours = 0;
  int minutes = 0;
  double seconds = 0;
  // "mm:ss.mmm" or "hh:mm:ss.mmm" (the spec's hours group is optional).
  if (std::sscanf(std::string(stamp).c_str(), "%d:%d:%lf", &hours, &minutes,
                  &seconds) == 3) {
    return base::Milliseconds(static_cast<int64_t>(
        (hours * 3600 + minutes * 60 + seconds) * 1000.0 + 0.5));
  }
  if (std::sscanf(std::string(stamp).c_str(), "%d:%lf", &minutes, &seconds) ==
      2) {
    return base::Milliseconds(
        static_cast<int64_t>((minutes * 60 + seconds) * 1000.0 + 0.5));
  }
  return base::unexpected(MediaError::Of(
      ErrorCode::kSourceUnsupported, "bad webvtt timestamp",
      "stamp = " + std::string(stamp),
      "expected mm:ss.mmm or hh:mm:ss.mmm -- check that the file is "
      "really WebVTT"));
}

base::expected<std::vector<TimedTextCue>, MediaError>
ParseWebVtt(std::string_view text, int source_id) {
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
  if (lines.empty() || !(lines[0].starts_with("WEBVTT") ||
                         (lines[0].size() >= 6 && lines[0].starts_with("﻿") &&
                          lines[0].substr(3).starts_with("WEBVTT")))) {
    return base::unexpected(MediaError::Of(
        ErrorCode::kSourceUnsupported, "not a webvtt file",
        "first line = " +
            std::string(lines.empty() ? "" : lines[0].substr(0, 32)),
        "check the file's content-type and encoding"));
  }

  std::vector<TimedTextCue> cues;
  base::TimeDelta cue_start;
  base::TimeDelta cue_end;
  std::string cue_text;
  bool in_cue = false;
  bool seen_signature = false;
  for (const std::string_view& line : lines) {
    if (!seen_signature) {
      seen_signature = true;  // line 0 was validated above
      continue;
    }
    if (!in_cue) {
      if (line.starts_with("NOTE") || line.starts_with("STYLE") ||
          line.starts_with("REGION")) {
        continue;  // block headers; their bodies are blank-line separated
      }
      auto arrow = line.find("-->");
      if (arrow == std::string_view::npos) {
        continue;  // cue identifier line, blank line, or settings junk
      }
      auto parsed_start = ParseVttTimestamp(line.substr(0, arrow));
      if (!parsed_start.has_value()) {
        continue;  // an --> inside text we are not in: not ours
      }
      std::string_view tail = line.substr(arrow + 3);
      const size_t tail_start = tail.find_first_not_of(" \t");
      if (tail_start == std::string_view::npos) {
        return base::unexpected(MediaError::Of(
            ErrorCode::kSourceUnsupported, "vtt cue has no end timestamp",
            "line = " + std::string(line),
            "expected 'start --> end' on the timing line"));
      }
      auto parsed_end = ParseVttTimestamp(tail.substr(tail_start));
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

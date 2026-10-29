// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FILTERS_TEXT_TAG_STRIP_H_
#define AVBASE_MEDIA_FILTERS_TEXT_TAG_STRIP_H_

#include <string>
#include <string_view>

namespace avbase::media::internal {

// Strips the <tag> shapes real subtitle files carry (<i>, </b>,
// <font ...>): cue text goes to the renderer as plain text, which is what
// the embedded text leg delivers for the same formats. Shared by the SRT
// and WebVTT parsers -- one copy, because the two files' tag grammars are
// identical and a fix (say, for unclosed tags) must not land in one only.
inline std::string StripTags(std::string_view line) {
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

}  // namespace avbase::media::internal

#endif  // AVBASE_MEDIA_FILTERS_TEXT_TAG_STRIP_H_

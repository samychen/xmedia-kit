// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/option_value_parsing.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace xmedia {
namespace option_internal {

// Edit distance used for the "did you mean" hint. ijkplayer silently ignores
// an unknown key, which is why a typo'd option can ship unnoticed (Δ2).
int Levenshtein(std::string_view a, std::string_view b) {
  if (a.empty())
    return static_cast<int>(b.size());
  if (b.empty())
    return static_cast<int>(a.size());
  std::vector<int> prev(b.size() + 1), cur(b.size() + 1);
  for (size_t j = 0; j <= b.size(); ++j)
    prev[j] = static_cast<int>(j);
  for (size_t i = 1; i <= a.size(); ++i) {
    cur[0] = static_cast<int>(i);
    for (size_t j = 1; j <= b.size(); ++j) {
      const int cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      cur[j] = std::min({prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + cost});
    }
    prev.swap(cur);
  }
  return prev[b.size()];
}

base::unexpected<MediaError> UnknownKey(std::string_view key,
                                        std::string_view suggestion) {
  std::string detail("key = \"");
  detail.append(key);
  detail.push_back('"');
  if (!suggestion.empty()) {
    detail.append("\n           did you mean \"");
    detail.append(suggestion);
    detail.append("\"?");
  }
  return Err(ErrorCode::kInvalidArgument, "unknown option key", detail,
             "call OptionRegistry::GetInstance().Describe() for the full list, "
             "or use the typed PlayerConfig field instead");
}

base::unexpected<MediaError> OutOfRange(std::string_view key,
                                        const std::string& got,
                                        const std::string& allowed) {
  std::string detail("key = \"");
  detail.append(key);
  detail.append("\"   value = ").append(got);
  detail.append("\n           allowed = ").append(allowed);
  return Err(ErrorCode::kInvalidArgument, "option value out of range", detail,
             "clamp the value, or remove the override and use the default");
}

base::unexpected<MediaError> TypeMismatch(std::string_view key,
                                          const std::string& expected) {
  std::string detail("key = \"");
  detail.append(key);
  detail.append("\" expects ").append(expected);
  return Err(ErrorCode::kInvalidArgument, "option type mismatch", detail,
             "pass the value with the matching type");
}

// ijkplayer exposes booleans as "1"/"0" (or true/false) and accepts them from
// int- and string-typed setters. Parse any of those so a migrated call site
// works regardless of which typed entry point it uses. Returns nullopt for an
// unrecognized string, so the caller can raise a type-mismatch error.
std::optional<bool> ParseBoolOption(const OptionValue& v) {
  if (const bool* b = std::get_if<bool>(&v)) {
    return *b;
  }
  if (const int64_t* i = std::get_if<int64_t>(&v)) {
    return *i != 0;
  }
  if (const std::string* s = std::get_if<std::string>(&v)) {
    if (*s == "1" || *s == "true" || *s == "on" || *s == "yes") {
      return true;
    }
    if (*s == "0" || *s == "false" || *s == "off" || *s == "no" || s->empty()) {
      return false;
    }
  }
  return std::nullopt;
}

}  // namespace option_internal
}  // namespace xmedia

// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The vocabulary the option table is written in: the three error shapes every
// key can raise, the boolean parser shared by every bool-typed key, and the
// edit distance behind "did you mean". Split out of option_registry.cc when
// the table plus its helpers crossed C1's 394-line ratchet -- the table and the
// helpers are two different jobs (naming what a key means vs. how a value is
// read), and this is the seam the file already had.

#ifndef XMEDIA_PLAYER_OPTION_VALUE_PARSING_H_
#define XMEDIA_PLAYER_OPTION_VALUE_PARSING_H_

#include <optional>
#include <string>
#include <string_view>

#include "player/public/error.h"
#include "player/public/option_registry.h"

namespace xmedia {
namespace option_internal {

int Levenshtein(std::string_view a, std::string_view b);

base::unexpected<MediaError> UnknownKey(std::string_view key,
                                        std::string_view suggestion);
base::unexpected<MediaError> OutOfRange(std::string_view key,
                                        const std::string& got,
                                        const std::string& allowed);
base::unexpected<MediaError> TypeMismatch(std::string_view key,
                                          const std::string& expected);
std::optional<bool> ParseBoolOption(const OptionValue& v);

}  // namespace option_internal
}  // namespace xmedia

#endif  // XMEDIA_PLAYER_OPTION_VALUE_PARSING_H_

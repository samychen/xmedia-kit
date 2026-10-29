// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FFMPEG_HLS_FETCH_UTILS_H_
#define AVBASE_MEDIA_FFMPEG_HLS_FETCH_UTILS_H_

#include <stdint.h>

#include <mutex>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "media/base/media_error.h"
#include "media/media_export.h"

namespace avbase::media {

class DataSource;  // the abort handle registers the fetch in flight

// The fetch half shared by the HLS demuxer's TUs and ClearKeyProvider --
// one copy of "GET this URL to completion" and "GET this key URI with a
// sane-size cap", so the open chain, the playlist fetcher and the default
// key path cannot drift apart.
//
// THREADING. Both functions block; call them from a worker thread. The
// caller owns |*abort_handle| (a DataSource registered under |abort_lock|
// for the duration of the fetch, so an external Stop() can Abort() it) --
// pass nullptr to opt out, as ClearKeyProvider does.
namespace hls_fetch {

// GETs |url| fully. Aborted or empty fetches return false; callers turn
// that into a structured error where they know what was being fetched.
AVBASE_MEDIA_EXPORT bool FetchWholeUrl(const std::string& url,
                                       base::raw_ptr<DataSource>* abort_handle,
                                       std::mutex* abort_lock,
                                       std::string* out);

// Plain key fetch: reads the whole body with a 4 KiB sanity cap -- an
// oversized body is DETECTED ("not a key"), never silently truncated --
// and an empty body or transport failure is a structured error.
AVBASE_MEDIA_EXPORT base::expected<std::vector<uint8_t>, MediaError>
FetchClearKeyBytes(const std::string& key_uri,
                   base::raw_ptr<DataSource>* abort_handle,
                   std::mutex* abort_lock);

// "0x"-prefixed 32-hex-digit IV -> 16 raw bytes; empty on anything else
// (the caller then falls back to the sequence-number default IV). The one
// hex parser of the HLS layer.
AVBASE_MEDIA_EXPORT std::string IvFromHex(const std::string& hex);

// UTF-8 text -> raw bytes (segment bodies arrive as strings).
AVBASE_MEDIA_EXPORT std::vector<uint8_t> StringBytes(const std::string& text);

}  // namespace hls_fetch

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FFMPEG_HLS_FETCH_UTILS_H_

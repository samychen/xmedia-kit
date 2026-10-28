// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/clear_key_provider.h"

#include "media/ffmpeg/url_data_source.h"

namespace avbase::media {

base::expected<std::vector<uint8_t>, MediaError>
ClearKeyProvider::GetKeyBlocking(const std::string& key_uri) {
  auto source = base::MakeRefCounted<ffmpeg::UrlDataSource>(key_uri);
  std::vector<uint8_t> bytes;
  int64_t offset = 0;
  // Keys are tiny (16 bytes for AES-128); read one byte past the sane
  // ceiling so an oversized body is DETECTED, not silently truncated.
  constexpr size_t kMaxKeyBytes = 4096;
  bool oversized = false;
  while (offset <= static_cast<int64_t>(kMaxKeyBytes)) {
    uint8_t buffer[1024];
    auto result = source->ReadBlocking(offset, sizeof(buffer), buffer);
    if (!result.has_value()) {
      return base::unexpected(MediaError::Of(
          ErrorCode::kSourceReadFailed, "the key could not be fetched",
          "url = " + key_uri + ", " + result.error().summary(),
          "check the EXT-X-KEY uri, or inject a KeyProvider for this "
          "key system"));
    }
    if (*result == 0) {
      break;
    }
    bytes.insert(bytes.end(), buffer, buffer + *result);
    offset += *result;
    if (bytes.size() > kMaxKeyBytes) {
      oversized = true;
      break;
    }
  }
  if (oversized) {
    return base::unexpected(MediaError::Of(
        ErrorCode::kSourceUnsupported, "the key response is not a key",
        "url = " + key_uri + ", body >= " + std::to_string(bytes.size()) +
            " bytes",
        "check that the key uri points at the key bytes, not at a page"));
  }
  if (bytes.empty()) {
    return base::unexpected(MediaError::Of(
        ErrorCode::kSourceUnsupported, "the key response was empty",
        "url = " + key_uri,
        "check that the key server answers the bytes, not a redirect "
        "or an error page"));
  }
  return bytes;
}

}  // namespace avbase::media

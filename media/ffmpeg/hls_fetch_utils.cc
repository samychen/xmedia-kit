// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/hls_fetch_utils.h"

#include "media/ffmpeg/url_data_source.h"

namespace avbase::media {
namespace hls_fetch {
namespace {

constexpr size_t kMaxKeyBytes = 4096;

}  // namespace

bool FetchWholeUrl(const std::string& url,
                   base::raw_ptr<DataSource>* abort_handle,
                   std::mutex* abort_lock, std::string* out) {
  auto source = base::MakeRefCounted<ffmpeg::UrlDataSource>(url);
  if (abort_handle != nullptr) {
    std::lock_guard<std::mutex> scoped(*abort_lock);
    *abort_handle = source.get();
  }
  std::string bytes;
  int64_t offset = 0;
  while (true) {
    char buffer[64 * 1024];
    auto result = source->ReadBlocking(offset, sizeof(buffer),
                                       reinterpret_cast<uint8_t*>(buffer));
    if (!result.has_value() || *result == 0) {
      break;
    }
    bytes.append(buffer, static_cast<size_t>(*result));
    offset += *result;
  }
  if (abort_handle != nullptr) {
    std::lock_guard<std::mutex> scoped(*abort_lock);
    *abort_handle = nullptr;
  }
  if (bytes.empty()) {
    return false;
  }
  *out = std::move(bytes);
  return true;
}

base::expected<std::vector<uint8_t>, MediaError>
FetchClearKeyBytes(const std::string& key_uri,
                   base::raw_ptr<DataSource>* abort_handle,
                   std::mutex* abort_lock) {
  auto source = base::MakeRefCounted<ffmpeg::UrlDataSource>(key_uri);
  if (abort_handle != nullptr) {
    std::lock_guard<std::mutex> scoped(*abort_lock);
    *abort_handle = source.get();
  }
  std::vector<uint8_t> bytes;
  int64_t offset = 0;
  // Keys are tiny (16 bytes for AES-128); read one byte past the sane
  // ceiling so an oversized body is DETECTED, not silently truncated.
  bool oversized = false;
  while (offset <= static_cast<int64_t>(kMaxKeyBytes)) {
    uint8_t buffer[1024];
    auto result = source->ReadBlocking(offset, sizeof(buffer), buffer);
    if (!result.has_value()) {
      if (abort_handle != nullptr) {
        std::lock_guard<std::mutex> scoped(*abort_lock);
        *abort_handle = nullptr;
      }
      return base::unexpected(MediaError::Of(
          ErrorCode::kSourceReadFailed, "the key could not be fetched",
          "url = " + key_uri + ", " + result.error().summary(),
          "check the EXT-X-KEY uri, or inject a KeyProvider for this key "
          "system"));
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
  if (abort_handle != nullptr) {
    std::lock_guard<std::mutex> scoped(*abort_lock);
    *abort_handle = nullptr;
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
        "check that the key server answers the bytes, not a redirect or "
        "an error page"));
  }
  return bytes;
}

std::string IvFromHex(const std::string& hex) {
  if (hex.size() != 34 || !(hex.starts_with("0x") || hex.starts_with("0X"))) {
    return {};
  }
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  std::string out;
  for (size_t i = 2; i + 1 < hex.size(); i += 2) {
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return {};
    }
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

std::vector<uint8_t> StringBytes(const std::string& text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

}  // namespace hls_fetch
}  // namespace avbase::media

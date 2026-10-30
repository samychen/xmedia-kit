// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/clear_key_provider.h"

#include <mutex>

#include "media/ffmpeg/hls_fetch_utils.h"

namespace xmedia::media {

base::expected<std::vector<uint8_t>, MediaError>
ClearKeyProvider::GetKeyBlocking(const std::string& key_uri) {
  // No external abort handle: the provider is owned by the host, and this
  // layer does not wire into the player's Stop path (the HLS demuxer's
  // built-in key fetch does, through its own abort handle registration).
  std::mutex unused_lock;
  return hls_fetch::FetchClearKeyBytes(key_uri, nullptr, &unused_lock);
}

}  // namespace xmedia::media

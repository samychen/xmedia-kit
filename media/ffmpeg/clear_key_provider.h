// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FFMPEG_CLEAR_KEY_PROVIDER_H_
#define AVBASE_MEDIA_FFMPEG_CLEAR_KEY_PROVIDER_H_

#include <string>
#include <vector>

#include "media/base/key_provider.h"
#include "media/media_export.h"

namespace avbase::media {

// The default KeyProvider: plain fetch of the key URI -- HLS's AES-128
// "clear key" flow, the CicadaPlayer counterpart being its inline
// EXT-X-KEY download plus the setDrmRequestCallback hook a product would
// override. The implementation lives in media/ffmpeg because it fetches
// through the avio-backed UrlDataSource (C27 keeps libav out of media/base,
// where the interface lives).
class AVBASE_MEDIA_EXPORT ClearKeyProvider final : public KeyProvider {
 public:
  ClearKeyProvider() = default;
  ClearKeyProvider(const ClearKeyProvider&) = delete;
  ClearKeyProvider& operator=(const ClearKeyProvider&) = delete;
  ~ClearKeyProvider() override = default;

  // KeyProvider:
  base::expected<std::vector<uint8_t>, MediaError>
  GetKeyBlocking(const std::string& key_uri) override;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FFMPEG_CLEAR_KEY_PROVIDER_H_

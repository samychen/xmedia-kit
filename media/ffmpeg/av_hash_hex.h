// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FFMPEG_AV_HASH_HEX_H_
#define XMEDIA_MEDIA_FFMPEG_AV_HASH_HEX_H_

// av_hash 的十六进制摘要包装：产物验源（MP4 → HTML → spec 哈希链）用。
// 本构建的 hash.h 无 C++ 守卫——extern "C" 手工包（同 swscale 处理）。

#include "media/ffmpeg/av_includes.h"

extern "C" {
#include <libavutil/hash.h>
}

#include <string>

namespace xmedia {
namespace media {
namespace ffmpeg {

// SHA-256 → 小写 hex（64 字符）。
inline std::string Sha256Hex(const std::string& data) {
  AVHashContext* hash = nullptr;
  std::string hex;
  if (av_hash_alloc(&hash, "SHA256") < 0 || !hash) {
    return hex;
  }
  av_hash_init(hash);
  av_hash_update(hash, reinterpret_cast<const uint8_t*>(data.data()),
                 data.size());
  uint8_t digest[AV_HASH_MAX_SIZE];
  av_hash_final(hash, digest);
  const int digest_size = av_hash_get_size(hash);
  av_hash_freep(&hash);
  char byte_hex[3];
  for (int i = 0; i < digest_size; ++i) {
    snprintf(byte_hex, sizeof(byte_hex), "%02x", digest[i]);
    hex += byte_hex;
  }
  return hex;
}

}  // namespace ffmpeg
}  // namespace media
}  // namespace xmedia

#endif  // XMEDIA_MEDIA_FFMPEG_AV_HASH_HEX_H_

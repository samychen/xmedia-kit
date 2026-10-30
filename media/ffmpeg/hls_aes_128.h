// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FFMPEG_HLS_AES_128_H_
#define XMEDIA_MEDIA_FFMPEG_HLS_AES_128_H_

#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "media/base/media_error.h"
#include "media/media_export.h"

namespace xmedia::media::hls {

// AES-128-CBC segment decryption for EXT-X-KEY:METHOD=AES-128 -- the port of
// CicadaPlayer's play_list/segment_decrypt/AES_128Decrypter. HLS prescribes
// whole-block PKCS7-padded CBC over the entire segment body, with the IV
// either from the playlist (EXT-X-KEY:IV) or derived from the segment's
// media sequence number (the spec's default: 64-bit big-endian sequence in
// the low half of a zero IV). SequenceNumberIv() builds that default.
class XMEDIA_MEDIA_EXPORT Aes128CbcDecrypter {
 public:
  // 16-byte key and IV; nullopt when the key length is wrong (the av_aes
  // init inside is what actually rejects, but a wrong-size key is always a
  // caller bug worth failing before any buffer is touched).
  static std::unique_ptr<Aes128CbcDecrypter> Create(const std::string& key,
                                                    const std::string& iv);

  // The spec's default IV: the segment's media sequence number, big-endian,
  // in the low 8 bytes of a zero IV.
  static std::string SequenceNumberIv(int64_t media_sequence);

  ~Aes128CbcDecrypter();

  // Decrypts one segment body. Returns the unpadded plaintext (empty input
  // decrypts to empty; a length that is not a whole number of 16-byte
  // blocks, or bad PKCS7 padding, is a structured error).
  base::expected<std::vector<uint8_t>, MediaError>
  Decrypt(const std::vector<uint8_t>& cipher);

 private:
  Aes128CbcDecrypter() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xmedia::media::hls

#endif  // XMEDIA_MEDIA_FFMPEG_HLS_AES_128_H_

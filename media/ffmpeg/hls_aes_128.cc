// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/hls_aes_128.h"

#include "av_includes.h"  // NOLINT: the wrap point every ffmpeg TU shares
extern "C" {
#include <libavutil/aes.h>  // this build's aes.h has no own extern "C"
}

namespace avbase::media::hls {
namespace {

constexpr size_t kBlock = 16;

}  // namespace

struct Aes128CbcDecrypter::Impl {
  // av_aes_alloc() heap context: AVAES is complete in <libavutil/aes.h> but
  // its layout is version-dependent, so a pointer plus av_free() beats a
  // by-value member.
  AVAES* aes = nullptr;
  uint8_t iv[kBlock] = {};
};

std::unique_ptr<Aes128CbcDecrypter>
Aes128CbcDecrypter::Create(const std::string& key, const std::string& iv) {
  if (key.size() != kBlock || iv.size() != kBlock) {
    return nullptr;
  }
  // |key| and |iv| arrive as raw bytes: the sequence-number default comes
  // from SequenceNumberIv, and the playlist's 0x-prefixed IV is expanded by
  // the caller (the HLS demuxer's IvFromHex).
  auto decrypter =
      std::unique_ptr<Aes128CbcDecrypter>(new Aes128CbcDecrypter());
  decrypter->impl_ = std::make_unique<Impl>();
  decrypter->impl_->aes = av_aes_alloc();
  if (decrypter->impl_->aes == nullptr ||
      av_aes_init(decrypter->impl_->aes,
                  reinterpret_cast<const uint8_t*>(key.data()), 128,
                  /*decrypt=*/1) != 0) {
    return nullptr;
  }
  memcpy(decrypter->impl_->iv, iv.data(), kBlock);
  return decrypter;
}

std::string Aes128CbcDecrypter::SequenceNumberIv(int64_t media_sequence) {
  std::string iv(kBlock, '\0');
  // Big-endian sequence number in the LOW 8 bytes (the RFC 8216 default).
  for (int i = 0; i < 8; ++i) {
    iv[15 - i] = static_cast<char>((media_sequence >> (i * 8)) & 0xFF);
  }
  return iv;
}

Aes128CbcDecrypter::~Aes128CbcDecrypter() {
  if (impl_ != nullptr && impl_->aes != nullptr) {
    av_free(impl_->aes);
  }
}

base::expected<std::vector<uint8_t>, MediaError>
Aes128CbcDecrypter::Decrypt(const std::vector<uint8_t>& cipher) {
  if (cipher.empty()) {
    return std::vector<uint8_t>();
  }
  if (cipher.size() % kBlock != 0) {
    return base::unexpected(MediaError::Of(
        ErrorCode::kSourceUnsupported,
        "the encrypted segment is not block aligned",
        "size = " + std::to_string(cipher.size()) + ", block = 16",
        "check that the server encrypts whole AES-128 blocks"));
  }
  std::vector<uint8_t> plain(cipher.size());
  // av_aes_crypt consumes |iv| in place, but each Decrypt() starts from the
  // playlist-provided IV, so work on a copy.
  uint8_t iv[kBlock];
  memcpy(iv, impl_->iv, kBlock);
  av_aes_crypt(impl_->aes, plain.data(),
               reinterpret_cast<const uint8_t*>(cipher.data()),
               static_cast<int>(cipher.size() / kBlock), iv, 0);

  // PKCS7: the last byte is the pad length 1..16, all pad bytes equal.
  const uint8_t pad = plain.back();
  if (pad < 1 || pad > kBlock) {
    return base::unexpected(MediaError::Of(
        ErrorCode::kSourceUnsupported, "bad pkcs7 padding",
        "last byte = " + std::to_string(pad),
        "check the key and iv against the playlist's EXT-X-KEY"));
  }
  for (size_t i = plain.size() - pad; i < plain.size(); ++i) {
    if (plain[i] != pad) {
      return base::unexpected(MediaError::Of(
          ErrorCode::kSourceUnsupported, "bad pkcs7 padding",
          "pad byte mismatch at " + std::to_string(i),
          "check the key and iv against the playlist's EXT-X-KEY"));
    }
  }
  plain.resize(plain.size() - pad);
  return plain;
}

}  // namespace avbase::media::hls

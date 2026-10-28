// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/hls_aes_128.h"

#include <string>
#include <vector>

#include "media/ffmpeg/av_includes.h"
extern "C" {
#include <libavutil/aes.h>  // this build's aes.h has no own extern "C"
}

#include "gtest/gtest.h"

namespace avbase::media::hls {
namespace {

// Binary keys/IVs: std::string(char*) would truncate at the first NUL, so
// every conversion below uses the (ptr, 16) form through these helpers.
constexpr char kKeyBytes[] =
    "\x2b\x7e\x15\x16\x28\xae\xd2\xa6\xab\xf7\x15\x88"
    "\x09\xcf\x4f\x3c";
constexpr char kIvBytes[] =
    "\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b"
    "\x0c\x0d\x0e\x0f";
std::string kKey() {
  return std::string(kKeyBytes, 16);
}
std::string kIv() {
  return std::string(kIvBytes, 16);
}
// NIST SP 800-38A CBC-AES128, first plaintext block.
constexpr char kPlaintextBlock[] =
    "\x6b\xc1\xbe\xe2\x2e\x40\x9f\x96"
    "\xe9\x3d\x7e\x11\x73\x93\x17\x2a";

std::string HexToBytes(std::string_view hex) {
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
  std::string bytes;
  for (size_t i = 0; i + 1 < hex.size(); i += 2) {
    bytes.push_back(
        static_cast<char>((nibble(hex[i]) << 4) | nibble(hex[i + 1])));
  }
  return bytes;
}

// Encrypts |plain| (already a whole number of blocks) with av_aes so the
// decrypter under test has a real producer instead of hardcoded blobs.
std::vector<uint8_t> EncryptCbc(const std::string& key, const std::string& iv,
                                const std::vector<uint8_t>& plain) {
  AVAES* ctx = av_aes_alloc();
  av_aes_init(ctx, reinterpret_cast<const uint8_t*>(key.data()), 128, 0);
  uint8_t work_iv[16];
  memcpy(work_iv, iv.data(), 16);
  std::vector<uint8_t> cipher(plain.size());
  av_aes_crypt(ctx, cipher.data(), plain.data(),
               static_cast<int>(plain.size() / 16), work_iv, 0);
  av_free(ctx);
  return cipher;
}

std::vector<uint8_t> Padded(const std::string& body) {
  std::vector<uint8_t> plain(body.begin(), body.end());
  const uint8_t pad = static_cast<uint8_t>(16 - (plain.size() % 16));
  plain.insert(plain.end(), pad, pad);
  return plain;
}

TEST(HlsAes128Test, DecryptsTheNistVector) {
  // One block, no padding yet: pad it with a full PKCS7 block so the
  // decrypter's unpad step sees a valid body.
  std::vector<uint8_t> plain(kPlaintextBlock, kPlaintextBlock + 16);
  plain.insert(plain.end(), 16, 16);  // PKCS7 full block
  auto cipher = EncryptCbc(kKey(), kIv(), plain);
  // The first ciphertext block must be the NIST published value -- this is
  // what proves the encrypt side (and hence the fixture) is honest.
  std::string expected_ct = HexToBytes("7649abac8119b246cee98e9b12e9197d");
  ASSERT_EQ(cipher.size(), 32u);
  EXPECT_EQ(memcmp(cipher.data(), expected_ct.data(), 16), 0);

  auto decrypter = Aes128CbcDecrypter::Create(kKey(), kIv());
  ASSERT_NE(decrypter, nullptr);
  auto result = decrypter->Decrypt(cipher);
  ASSERT_TRUE(result.has_value()) << result.error().summary();
  ASSERT_EQ(result->size(), 16u);
  EXPECT_EQ(memcmp(result->data(), kPlaintextBlock, 16), 0);
}

TEST(HlsAes128Test, RoundTripsMultiBlockBodies) {
  const std::string body =
      "segment bytes that span several aes blocks for padding edge cases";
  auto cipher = EncryptCbc(kKey(), kIv(), Padded(body));
  auto decrypter = Aes128CbcDecrypter::Create(kKey(), kIv());
  ASSERT_NE(decrypter, nullptr);
  auto result = decrypter->Decrypt(cipher);
  ASSERT_TRUE(result.has_value()) << result.error().summary();
  std::string decrypted(result->begin(), result->end());
  EXPECT_EQ(decrypted, body);
}

TEST(HlsAes128Test, SequenceNumberIvFollowsTheSpecDefault) {
  std::string iv0 = Aes128CbcDecrypter::SequenceNumberIv(0);
  EXPECT_EQ(iv0, std::string(16, '\0'));
  std::string iv1 = Aes128CbcDecrypter::SequenceNumberIv(1);
  EXPECT_EQ(iv1.back(), '\x01');
  std::string iv256 = Aes128CbcDecrypter::SequenceNumberIv(256);
  EXPECT_EQ(iv256[14], '\x01');
  EXPECT_EQ(iv256[15], '\x00');
}

TEST(HlsAes128Test, RejectsWrongKeyLengthAndBadPadding) {
  EXPECT_EQ(Aes128CbcDecrypter::Create("short", kIv()), nullptr);
  auto decrypter = Aes128CbcDecrypter::Create(kKey(), kIv());
  ASSERT_NE(decrypter, nullptr);
  // Not a whole number of blocks.
  auto misaligned = decrypter->Decrypt(std::vector<uint8_t>(17, 0xAB));
  ASSERT_FALSE(misaligned.has_value());
  // Whole blocks, nonsense padding.
  auto bad_pad = decrypter->Decrypt(
      EncryptCbc(kKey(), kIv(), std::vector<uint8_t>(16, 0)));
  ASSERT_FALSE(bad_pad.has_value());
  EXPECT_EQ(bad_pad.error().code(), ErrorCode::kSourceUnsupported);
}

}  // namespace
}  // namespace avbase::media::hls

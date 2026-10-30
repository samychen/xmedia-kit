// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/hls_aes_128.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace xmedia::media::hls {
namespace {

// The key and IV of NIST SP 800-38A CBC-AES128; the ciphertexts below were
// produced against them with openssl (the production layer has no encryptor
// to test against, and C4 keeps libav headers out of the tests).
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

std::vector<uint8_t> BytesOf(const std::string& text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

TEST(HlsAes128Test, DecryptsTheNistVector) {
  // PT block 6bc1bee2... + a full PKCS7 pad block, per the published vector.
  auto cipher = BytesOf(HexToBytes(
      "7649abac8119b246cee98e9b12e9197d8964e0b149c10b7b682e6e39aaeb731c"));
  auto decrypter = Aes128CbcDecrypter::Create(kKey(), kIv());
  ASSERT_NE(decrypter, nullptr);
  auto result = decrypter->Decrypt(cipher);
  ASSERT_TRUE(result.has_value()) << result.error().summary();
  ASSERT_EQ(result->size(), 16u);
  const std::string expected_pt =
      HexToBytes("6bc1bee22e409f96e93d7e117393172a");
  EXPECT_EQ(memcmp(result->data(), expected_pt.data(), 16), 0);
}

TEST(HlsAes128Test, RoundTripsAMultiBlockBody) {
  // openssl-encrypted body "segment bytes that span several aes blocks for
  // padding edge cases" (PKCS7 padded by the producer).
  auto cipher = BytesOf(HexToBytes(
      "77e8ff925474aa6dd762a2e7972bed471ff530ed350f7da05ee70745e25016c6"
      "add8545fbea1b9b18cbe01dfd8c3087a2c2aa2027af55e6ce128eab47dbfb6d5"
      "1e3938db99af598a165f57cf5a45fd7d"));
  auto decrypter = Aes128CbcDecrypter::Create(kKey(), kIv());
  ASSERT_NE(decrypter, nullptr);
  auto result = decrypter->Decrypt(cipher);
  ASSERT_TRUE(result.has_value()) << result.error().summary();
  const std::string body(result->begin(), result->end());
  EXPECT_EQ(body,
            "segment bytes that span several aes blocks for padding edge "
            "cases");
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
  // One whole block whose plaintext is all zeros: PKCS7 says pad byte 0,
  // which no legal body ends with.
  auto bad_pad = decrypter->Decrypt(
      BytesOf(HexToBytes("50fe67cc996d32b6da0937e99bafec60")));
  ASSERT_FALSE(bad_pad.has_value());
  EXPECT_EQ(bad_pad.error().code(), ErrorCode::kSourceUnsupported);
}

}  // namespace
}  // namespace xmedia::media::hls

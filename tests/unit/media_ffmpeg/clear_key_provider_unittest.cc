// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/clear_key_provider.h"

#include "gtest/gtest.h"
#include "tests/support/local_http_server.h"

namespace avbase::media {
namespace {

class ClearKeyProviderTest : public ::testing::Test {
 public:
  void SetUp() override {
    if (!server_.Start()) {
      GTEST_SKIP() << "loopback sockets unavailable";
    }
    test::LocalHttpServer::File key;
    key.body.assign(16, '\x0a');
    server_.SetFile("/key.bin", key);
    test::LocalHttpServer::File huge;
    huge.body.assign(8192, 'x');
    server_.SetFile("/huge.bin", huge);
  }

  test::LocalHttpServer server_;
};

TEST_F(ClearKeyProviderTest, FetchesTheKeyBytes) {
  ClearKeyProvider provider;
  auto key = provider.GetKeyBlocking(server_.UrlFor("/key.bin"));
  ASSERT_TRUE(key.has_value()) << key.error().summary();
  EXPECT_EQ(key->size(), 16u);
  EXPECT_EQ((*key)[0], 0x0a);
}

TEST_F(ClearKeyProviderTest, MissingKeyIsAStructuredError) {
  ClearKeyProvider provider;
  auto key = provider.GetKeyBlocking(server_.UrlFor("/missing"));
  ASSERT_FALSE(key.has_value());
  EXPECT_EQ(key.error().code(), ErrorCode::kSourceReadFailed);
  EXPECT_FALSE(key.error().suggestion().empty());
}

TEST_F(ClearKeyProviderTest, AbsurdKeyBodyIsRejected) {
  ClearKeyProvider provider;
  // 8 KiB is not a key; the provider must stop reading and report, not
  // buffer a body an attacker replaced the key with.
  auto key = provider.GetKeyBlocking(server_.UrlFor("/huge.bin"));
  ASSERT_FALSE(key.has_value());
  EXPECT_EQ(key.error().code(), ErrorCode::kSourceUnsupported);
}

}  // namespace
}  // namespace avbase::media

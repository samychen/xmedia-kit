// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/curl/curl_data_source.h"

#include <stdint.h>

#include <thread>
#include <vector>

#include "base/time/time.h"
#include "gtest/gtest.h"
#include "tests/support/local_http_server.h"

namespace avbase::media::curl {
namespace {

using test::LocalHttpServer;

// 64 KiB of deterministic bytes: byte i is (i * 31 + 7) % 251, so a wrong
// offset can never look right.
std::string MakeBody(int64_t size) {
  std::string body;
  body.resize(static_cast<size_t>(size));
  for (int64_t i = 0; i < size; ++i) {
    body[static_cast<size_t>(i)] = static_cast<char>((i * 31 + 7) % 251);
  }
  return body;
}

class CurlDataSourceTest : public ::testing::Test {
 public:
  void SetUp() override {
    if (!server_.Start()) {
      GTEST_SKIP() << "loopback sockets unavailable";
    }
    body_ = MakeBody(kBodySize);
    LocalHttpServer::File file;
    file.body = body_;
    server_.SetFile("/video.bin", file);
  }

  CurlDataSource::Config DefaultConfig() const { return {}; }
  std::string Url() const { return server_.UrlFor("/video.bin"); }

  static constexpr int64_t kBodySize = 64 * 1024;
  LocalHttpServer server_;
  std::string body_;
};

TEST_F(CurlDataSourceTest, ReadsTheRequestedBytesSequentially) {
  auto source = base::MakeRefCounted<CurlDataSource>(Url(), DefaultConfig());
  std::vector<uint8_t> buffer(1000);
  auto result = source->ReadBlocking(0, buffer.size(), buffer.data());
  ASSERT_TRUE(result.has_value()) << result.error().summary();
  EXPECT_EQ(*result, 1000);
  for (size_t i = 0; i < buffer.size(); ++i) {
    ASSERT_EQ(buffer[i], static_cast<uint8_t>(body_[i])) << "at " << i;
  }
  auto second = source->ReadBlocking(1000, buffer.size(), buffer.data());
  ASSERT_TRUE(second.has_value()) << second.error().summary();
  EXPECT_EQ(*second, 1000);
  for (size_t i = 0; i < buffer.size(); ++i) {
    ASSERT_EQ(buffer[i], static_cast<uint8_t>(body_[1000 + i])) << "at " << i;
  }
}

TEST_F(CurlDataSourceTest, ReadsShortNearTheEndAndZeroPastIt) {
  auto source = base::MakeRefCounted<CurlDataSource>(Url(), DefaultConfig());
  std::vector<uint8_t> buffer(100);
  // Ask for more than remains: a partial read ending at the object size.
  auto tail =
      source->ReadBlocking(kBodySize - 10, buffer.size(), buffer.data());
  ASSERT_TRUE(tail.has_value()) << tail.error().summary();
  EXPECT_EQ(*tail, 10);
  for (int i = 0; i < 10; ++i) {
    EXPECT_EQ(buffer[static_cast<size_t>(i)],
              static_cast<uint8_t>(body_[kBodySize - 10 + i]));
  }
  auto eof = source->ReadBlocking(kBodySize, buffer.size(), buffer.data());
  ASSERT_TRUE(eof.has_value()) << eof.error().summary();
  EXPECT_EQ(*eof, 0);
}

TEST_F(CurlDataSourceTest, ReportsTheServerObjectSizeAndSeekability) {
  auto source = base::MakeRefCounted<CurlDataSource>(Url(), DefaultConfig());
  EXPECT_FALSE(source->IsSeekable());  // nothing observed yet
  std::vector<uint8_t> buffer(16);
  ASSERT_TRUE(
      source->ReadBlocking(0, buffer.size(), buffer.data()).has_value());
  int64_t size = -1;
  ASSERT_TRUE(source->GetSize(&size));
  EXPECT_EQ(size, kBodySize);
  EXPECT_TRUE(source->IsSeekable());
  EXPECT_FALSE(source->IsStreaming());
}

TEST_F(CurlDataSourceTest, SeeksToArbitraryOffsets) {
  auto source = base::MakeRefCounted<CurlDataSource>(Url(), DefaultConfig());
  std::vector<uint8_t> buffer(100);
  ASSERT_TRUE(
      source->ReadBlocking(50'000, buffer.size(), buffer.data()).has_value());
  for (size_t i = 0; i < buffer.size(); ++i) {
    ASSERT_EQ(buffer[i], static_cast<uint8_t>(body_[50'000 + i])) << "at " << i;
  }
  // Back to the start: a fresh Range request on the same connection.
  ASSERT_TRUE(
      source->ReadBlocking(0, buffer.size(), buffer.data()).has_value());
  for (size_t i = 0; i < buffer.size(); ++i) {
    ASSERT_EQ(buffer[i], static_cast<uint8_t>(body_[i])) << "at " << i;
  }
}

TEST_F(CurlDataSourceTest, KeepAliveServesEveryReadOverOneConnection) {
  auto source = base::MakeRefCounted<CurlDataSource>(Url(), DefaultConfig());
  std::vector<uint8_t> buffer(4096);
  for (int64_t offset = 0; offset < kBodySize; offset += 4096) {
    auto result = source->ReadBlocking(offset, buffer.size(), buffer.data());
    ASSERT_TRUE(result.has_value())
        << result.error().summary() << " at " << offset;
  }
  // One TCP connection for the whole session: curl's connection cache plus
  // the bounded range per read.
  EXPECT_EQ(server_.connection_count(), 1);
  EXPECT_GT(server_.request_count(), 1);
}

TEST_F(CurlDataSourceTest, CustomHeadersAndUserAgentReachTheServer) {
  CurlDataSource::Config config = DefaultConfig();
  config.user_agent = "avbase-curl-test/1.0";
  config.headers = {{"X-Test-Header", "hello"}};
  auto source = base::MakeRefCounted<CurlDataSource>(Url(), config);
  std::vector<uint8_t> buffer(16);
  ASSERT_TRUE(
      source->ReadBlocking(0, buffer.size(), buffer.data()).has_value());
  auto headers = server_.last_headers();
  EXPECT_EQ(headers["x-test-header"], "hello");
  EXPECT_EQ(headers["user-agent"], "avbase-curl-test/1.0");
  EXPECT_EQ(headers["range"], "bytes=0-15");
}

TEST_F(CurlDataSourceTest, MissingUrlSurfacesSourceNotFound) {
  auto source = base::MakeRefCounted<CurlDataSource>(server_.UrlFor("/missing"),
                                                     DefaultConfig());
  std::vector<uint8_t> buffer(16);
  auto result = source->ReadBlocking(0, buffer.size(), buffer.data());
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kSourceNotFound);
  EXPECT_FALSE(result.error().suggestion().empty());
}

TEST_F(CurlDataSourceTest, RangeIgnoringServerRefusesToSeek) {
  LocalHttpServer::File file;
  file.body = body_;
  file.ignore_range = true;
  server_.SetFile("/no-range.bin", file);
  auto source = base::MakeRefCounted<CurlDataSource>(
      server_.UrlFor("/no-range.bin"), DefaultConfig());
  std::vector<uint8_t> buffer(16);
  ASSERT_TRUE(
      source->ReadBlocking(0, buffer.size(), buffer.data()).has_value());
  EXPECT_FALSE(source->IsSeekable());
  auto jumped = source->ReadBlocking(5000, buffer.size(), buffer.data());
  ASSERT_FALSE(jumped.has_value());
  EXPECT_EQ(jumped.error().code(), ErrorCode::kSourceSeekFailed);
}

TEST_F(CurlDataSourceTest, AbortUnblocksAPendingRead) {
  LocalHttpServer::File drip;
  drip.body = body_;
  drip.drip_ms_per_chunk = 20;  // ~12.8 KB/s: above the low-speed watchdog.
  server_.SetFile("/drip.bin", drip);
  auto source = base::MakeRefCounted<CurlDataSource>(
      server_.UrlFor("/drip.bin"), DefaultConfig());
  std::vector<uint8_t> buffer(8192);
  std::thread aborter([&source] {
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    source->Abort();
  });
  const base::TimeTicks start = base::TimeTicks::Now();
  auto result = source->ReadBlocking(0, buffer.size(), buffer.data());
  const base::TimeDelta elapsed = base::TimeTicks::Now() - start;
  aborter.join();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kAborted);
  // The point of the progress-callback abort: Stop() must not park on a
  // stalled body (Delta 15). The drip alone would need the full body time.
  EXPECT_LT(elapsed, base::Seconds(5));
}

}  // namespace
}  // namespace avbase::media::curl

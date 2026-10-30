// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/file_cache_data_source.h"

#include <stdint.h>

#include <filesystem>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "media/base/data_source.h"
#include "media/base/media_error.h"

namespace xmedia::media {
namespace {

std::vector<uint8_t> MakeBody(size_t size) {
  std::vector<uint8_t> body(size);
  for (size_t i = 0; i < size; ++i) {
    body[i] = static_cast<uint8_t>((i * 29 + 11) % 251);
  }
  return body;
}

// Serves like MemoryDataSource but fails every read at/below a floor offset
// -- the stand-in for "the network is gone": only bytes the cache must NOT
// be able to reach (offsets above the floor) may touch it.
class NetworkGoneBelow final : public DataSource {
 public:
  NetworkGoneBelow(std::vector<uint8_t> body, int64_t floor)
      : body_(std::move(body)),
        floor_(floor),
        inner_(base::MakeRefCounted<MemoryDataSource>(body_.data(),
                                                      body_.size())) {}

  // ---- DataSource ----------------------------------------------------------
  void SetHost(Host* host) override { inner_->SetHost(host); }
  void Read(int64_t offset, size_t size, uint8_t* data,
            base::scoped_refptr<base::TaskRunner> task_runner,
            ReadCB read_cb) override {
    inner_->Read(offset, size, data, std::move(task_runner),
                 std::move(read_cb));
  }
  ReadResult ReadBlocking(int64_t offset, size_t size, uint8_t* data) override {
    if (offset <= floor_) {
      return base::unexpected(
          Err(ErrorCode::kNetworkUnreachable, "the network is gone below",
              "offset = " + std::to_string(offset),
              "the cache was supposed to serve this range"));
    }
    return inner_->ReadBlocking(offset, size, data);
  }
  void Abort() override { inner_->Abort(); }
  bool GetSize(int64_t* size_out) override { return inner_->GetSize(size_out); }
  bool IsStreaming() const override { return inner_->IsStreaming(); }
  void SetBitrate(int bitrate) override { inner_->SetBitrate(bitrate); }
  bool IsSeekable() const override { return inner_->IsSeekable(); }

 private:
  std::vector<uint8_t> body_;
  int64_t floor_;
  base::scoped_refptr<DataSource> inner_;
};

class FileCacheDataSourceTest : public ::testing::Test {
 public:
  void SetUp() override {
    body_ = MakeBody(16 * 1024);
    dir_ = (std::filesystem::temp_directory_path() /
            ("xmedia_file_cache_" + std::to_string(++instance_counter) +
             std::to_string(reinterpret_cast<intptr_t>(this))))
               .string();
    std::filesystem::create_directories(dir_);
  }
  void TearDown() override {
    std::error_code ignored;
    std::filesystem::remove_all(dir_, ignored);
  }

  FileCacheDataSource::Config ConfigFor(const std::string& url,
                                        size_t max_bytes) const {
    FileCacheDataSource::Config config;
    config.url = url;
    config.cache_dir = dir_;
    config.max_bytes = max_bytes;
    return config;
  }

 protected:
  static inline int instance_counter = 0;
  std::vector<uint8_t> body_;
  std::string dir_;
};

TEST_F(FileCacheDataSourceTest, PersistsEverythingItServes) {
  auto inner =
      base::MakeRefCounted<MemoryDataSource>(body_.data(), body_.size());
  FileCacheDataSource source(inner, ConfigFor("http://cdn/video.bin", 1 << 20));
  std::vector<uint8_t> buffer(4096);
  for (int64_t offset = 0; offset < static_cast<int64_t>(body_.size());
       offset += 4096) {
    auto result = source.ReadBlocking(offset, buffer.size(), buffer.data());
    ASSERT_TRUE(result.has_value()) << result.error().summary();
  }
  EXPECT_EQ(source.cached_bytes(), static_cast<int64_t>(body_.size()));
  EXPECT_TRUE(source.cache_active());
}

TEST_F(FileCacheDataSourceTest, SecondSessionServesThePrefixWithoutNetwork) {
  auto inner =
      base::MakeRefCounted<MemoryDataSource>(body_.data(), body_.size());
  {
    FileCacheDataSource first(inner,
                              ConfigFor("http://cdn/video.bin", 1 << 20));
    std::vector<uint8_t> buffer(4096);
    ASSERT_TRUE(
        first.ReadBlocking(0, buffer.size(), buffer.data()).has_value());
    ASSERT_TRUE(
        first.ReadBlocking(4096, buffer.size(), buffer.data()).has_value());
  }
  // New session, same URI: the network is gone for everything the first
  // session downloaded, and the read still succeeds from disk.
  auto gone = base::MakeRefCounted<NetworkGoneBelow>(body_, 8192);
  FileCacheDataSource second(gone, ConfigFor("http://cdn/video.bin", 1 << 20));
  EXPECT_TRUE(second.cache_active());
  std::vector<uint8_t> buffer(8192);
  auto result = second.ReadBlocking(0, buffer.size(), buffer.data());
  ASSERT_TRUE(result.has_value()) << result.error().summary();
  for (size_t i = 0; i < buffer.size(); ++i) {
    ASSERT_EQ(buffer[i], body_[i]);
  }
}

TEST_F(FileCacheDataSourceTest, ResumeAppendsTheTailBeyondTheMark) {
  {
    auto inner =
        base::MakeRefCounted<MemoryDataSource>(body_.data(), body_.size());
    FileCacheDataSource first(inner,
                              ConfigFor("http://cdn/video.bin", 1 << 20));
    std::vector<uint8_t> buffer(1024);
    // First session reads only the first 2 KiB.
    ASSERT_TRUE(
        first.ReadBlocking(0, buffer.size(), buffer.data()).has_value());
    ASSERT_TRUE(
        first.ReadBlocking(1024, buffer.size(), buffer.data()).has_value());
  }
  // Second session: network alive only ABOVE the mark (2 KiB) -- the prefix
  // must come from disk, the tail from the inner source, one continuous read.
  // The floor sits strictly inside the downloaded prefix: if the cache ever
  // served a below-mark byte from the network, this source fails the read.
  auto tail_only = base::MakeRefCounted<NetworkGoneBelow>(body_, 1535);
  FileCacheDataSource second(tail_only,
                             ConfigFor("http://cdn/video.bin", 1 << 20));
  std::vector<uint8_t> buffer(body_.size());
  auto result = second.ReadBlocking(0, buffer.size(), buffer.data());
  ASSERT_TRUE(result.has_value()) << result.error().summary();
  for (size_t i = 0; i < buffer.size(); ++i) {
    ASSERT_EQ(buffer[i], body_[i]);
  }
  EXPECT_EQ(second.cached_bytes(), static_cast<int64_t>(body_.size()));
}

TEST_F(FileCacheDataSourceTest, UnwritableDirectoryDegradesToReadThrough) {
  FileCacheDataSource::Config config =
      ConfigFor("http://cdn/video.bin", 1 << 20);
  config.cache_dir = "/dev/null/definitely/not/a/dir";
  auto inner =
      base::MakeRefCounted<MemoryDataSource>(body_.data(), body_.size());
  FileCacheDataSource source(inner, config);
  EXPECT_FALSE(source.cache_active());
  std::vector<uint8_t> buffer(1024);
  auto result = source.ReadBlocking(0, buffer.size(), buffer.data());
  ASSERT_TRUE(result.has_value()) << result.error().summary();
  for (size_t i = 0; i < buffer.size(); ++i) {
    ASSERT_EQ(buffer[i], body_[i]);
  }
}

TEST_F(FileCacheDataSourceTest, BudgetCapStopsAppendingButNotServing) {
  FileCacheDataSource::Config config = ConfigFor("http://cdn/video.bin", 4096);
  auto inner =
      base::MakeRefCounted<MemoryDataSource>(body_.data(), body_.size());
  FileCacheDataSource source(inner, config);
  std::vector<uint8_t> buffer(1024);
  for (int64_t offset = 0; offset < static_cast<int64_t>(body_.size());
       offset += 1024) {
    auto result = source.ReadBlocking(offset, buffer.size(), buffer.data());
    ASSERT_TRUE(result.has_value()) << result.error().summary();
  }
  EXPECT_EQ(source.cached_bytes(), 4096);
  // Everything past the budget was still served (read-through).
  auto tail = source.ReadBlocking(8192, buffer.size(), buffer.data());
  ASSERT_TRUE(tail.has_value()) << tail.error().summary();
  for (size_t i = 0; i < buffer.size(); ++i) {
    ASSERT_EQ(buffer[i], body_[8192 + i]);
  }
}

}  // namespace
}  // namespace xmedia::media

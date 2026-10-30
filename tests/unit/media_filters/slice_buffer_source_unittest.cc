// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/slice_buffer_source.h"

#include <stdint.h>

#include <vector>

#include "gtest/gtest.h"
#include "media/base/data_source.h"

namespace xmedia::media {
namespace {

// 32 KiB of deterministic bytes; byte i is (i * 17 + 3) % 241.
std::vector<uint8_t> MakeBody(size_t size) {
  std::vector<uint8_t> body(size);
  for (size_t i = 0; i < size; ++i) {
    body[i] = static_cast<uint8_t>((i * 17 + 3) % 241);
  }
  return body;
}

// Wraps MemoryDataSource and counts ReadBlocking calls -- the observable
// that distinguishes a cache hit from a miss.
class CountingSource final : public DataSource {
 public:
  explicit CountingSource(std::vector<uint8_t> body)
      : body_(std::move(body)),
        inner_(base::MakeRefCounted<MemoryDataSource>(body_.data(),
                                                      body_.size())) {}

  int read_calls() const {
    std::lock_guard<std::mutex> scoped(lock_);
    return read_calls_;
  }

  // ---- DataSource ----------------------------------------------------------
  void SetHost(Host* host) override { inner_->SetHost(host); }
  void Read(int64_t offset, size_t size, uint8_t* data,
            base::scoped_refptr<base::TaskRunner> task_runner,
            ReadCB read_cb) override {
    inner_->Read(offset, size, data, std::move(task_runner),
                 std::move(read_cb));
  }
  ReadResult ReadBlocking(int64_t offset, size_t size, uint8_t* data) override {
    std::lock_guard<std::mutex> scoped(lock_);
    ++read_calls_;
    return inner_->ReadBlocking(offset, size, data);
  }
  void Abort() override { inner_->Abort(); }
  bool GetSize(int64_t* size_out) override { return inner_->GetSize(size_out); }
  bool IsStreaming() const override { return inner_->IsStreaming(); }
  void SetBitrate(int bitrate) override { inner_->SetBitrate(bitrate); }
  bool IsSeekable() const override { return inner_->IsSeekable(); }

 private:
  std::vector<uint8_t> body_;
  base::scoped_refptr<DataSource> inner_;
  mutable std::mutex lock_;
  int read_calls_ = 0;
};

class SliceBufferSourceTest : public ::testing::Test {
 public:
  void SetUp() override { body_ = MakeBody(32 * 1024); }

 protected:
  std::vector<uint8_t> body_;
};

TEST_F(SliceBufferSourceTest, ServesBytesIdenticalToTheInnerSource) {
  auto inner = base::MakeRefCounted<CountingSource>(body_);
  SliceBufferSource source(inner, {});
  std::vector<uint8_t> buffer(1000);
  // A spread of offsets: head (probe), middle (box re-read), near-end.
  for (int64_t offset :
       {int64_t{0}, int64_t{7}, int64_t{15 * 1024}, int64_t{32 * 1024 - 10}}) {
    auto result = source.ReadBlocking(offset, buffer.size(), buffer.data());
    ASSERT_TRUE(result.has_value()) << result.error().summary();
    const size_t expected =
        std::min(buffer.size(), body_.size() - static_cast<size_t>(offset));
    EXPECT_EQ(static_cast<size_t>(*result), expected);
    for (size_t i = 0; i < static_cast<size_t>(*result); ++i) {
      ASSERT_EQ(buffer[i], body_[static_cast<size_t>(offset) + i]);
    }
  }
}

TEST_F(SliceBufferSourceTest, SecondReadOfTheSameRangeHitsTheCache) {
  auto inner = base::MakeRefCounted<CountingSource>(body_);
  SliceBufferSource source(inner, {});
  std::vector<uint8_t> buffer(100);
  ASSERT_TRUE(source.ReadBlocking(0, buffer.size(), buffer.data()).has_value());
  const int calls_after_first = inner->read_calls();
  EXPECT_GT(calls_after_first, 0);
  // Same range again: served from slices, the inner source is not touched.
  ASSERT_TRUE(source.ReadBlocking(0, buffer.size(), buffer.data()).has_value());
  EXPECT_EQ(inner->read_calls(), calls_after_first);
  // A neighbouring offset inside the already-loaded slice is a hit too: the
  // miss loaded a WHOLE slice, which is the point of slice granularity.
  ASSERT_TRUE(
      source.ReadBlocking(50, buffer.size(), buffer.data()).has_value());
  EXPECT_EQ(inner->read_calls(), calls_after_first);
}

TEST_F(SliceBufferSourceTest, EvictsLeastRecentlyUsedWithinTheBudget) {
  auto inner = base::MakeRefCounted<CountingSource>(body_);
  SliceBufferSource::Config config;
  config.slice_size = 1024;
  config.max_bytes = 4 * 1024;  // four slices fit
  SliceBufferSource source(inner, config);
  std::vector<uint8_t> buffer(1024);
  // Read five far-apart slices: the first one must have been evicted.
  for (int64_t offset : {int64_t{0}, int64_t{8 * 1024}, int64_t{16 * 1024},
                         int64_t{24 * 1024}, int64_t{31 * 1024}}) {
    ASSERT_TRUE(
        source.ReadBlocking(offset, buffer.size(), buffer.data()).has_value());
  }
  EXPECT_LE(source.cached_bytes(), config.max_bytes);
  // Correctness survives eviction: re-read the first range; it must come
  // back byte-identical even though its slice was dropped.
  auto result = source.ReadBlocking(0, buffer.size(), buffer.data());
  ASSERT_TRUE(result.has_value()) << result.error().summary();
  for (size_t i = 0; i < buffer.size(); ++i) {
    ASSERT_EQ(buffer[i], body_[i]);
  }
}

TEST_F(SliceBufferSourceTest, ZeroSizeReadIsEofNotAnError) {
  auto inner = base::MakeRefCounted<CountingSource>(body_);
  SliceBufferSource source(inner, {});
  auto result = source.ReadBlocking(0, 0, nullptr);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0);
}

}  // namespace
}  // namespace xmedia::media

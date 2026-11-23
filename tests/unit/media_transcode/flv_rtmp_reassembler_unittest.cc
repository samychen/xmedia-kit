// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/transcode/flv_rtmp_reassembler.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace xmedia::media {
namespace {

// Builds one FLV tag: 11-byte header (type + 24-bit body size + timestamp) +
// |body| bytes + 4-byte PreviousTagSize. Matches RTMP_Write's parser.
std::vector<uint8_t> MakeTag(uint8_t type, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> out;
  const size_t n = body.size();
  out.push_back(type);
  out.push_back(static_cast<uint8_t>((n >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((n >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>(n & 0xff));
  out.push_back(0);  // timestamp: 3 bytes + 1 extended byte.
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);
  out.push_back(0);  // stream id: 3 bytes.
  out.push_back(0);
  out.push_back(0);
  out.insert(out.end(), body.begin(), body.end());
  const uint32_t prev = static_cast<uint32_t>(n + 11);
  out.push_back(static_cast<uint8_t>((prev >> 24) & 0xff));
  out.push_back(static_cast<uint8_t>((prev >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((prev >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>(prev & 0xff));
  return out;
}

std::vector<uint8_t> MakeFlvHeader() {
  // 'FLV', version 1, flags, offset 9; then PreviousTagSize0 = 0.
  return {'F', 'L', 'V', 1, 5, 0, 0, 0, 9, 0, 0, 0, 0};
}

std::vector<uint8_t> Concat(const std::vector<uint8_t>& a,
                            const std::vector<uint8_t>& b) {
  std::vector<uint8_t> out = a;
  out.insert(out.end(), b.begin(), b.end());
  return out;
}

int FeedAll(FlvRtmpReassembler& r, const std::vector<uint8_t>& bytes) {
  return r.Feed(bytes.data(), bytes.size());
}

// The bug we are guarding: AVIO's 32 KiB flush boundary can split the 11-byte
// tag header. librtmp sees <11 bytes, returns 0, FFmpeg reports AVERROR_EOF.
// Feeding the header one byte at a time must yield nothing until the whole
// tag is present, then exactly one whole tag in a single write.
TEST(FlvRtmpReassemblerTest, SplitTagHeaderStillYieldsOneWholeTag) {
  const auto tag = MakeTag(9, {0x17, 0x01, 0x00, 0x00});  // 4-byte video tag.
  FlvRtmpReassembler r;

  int ready = FeedAll(r, MakeFlvHeader());
  EXPECT_EQ(ready, 0);  // Header alone is not enough: librtmp needs a tag.

  const std::vector<uint8_t> whole = tag;
  for (size_t i = 0; i < whole.size() - 1; ++i) {
    ready = r.Feed(&whole[i], 1);
    EXPECT_EQ(ready, 0) << "premature write at byte " << i;
  }
  ready = r.Feed(&whole.back(), 1);
  // librtmp needs FLV header + first tag in one write: 13 header bytes
  // plus the whole tag are emitted together.
  ASSERT_EQ(ready, static_cast<int>(13 + whole.size()));

  const auto& buf = r.buffer();
  ASSERT_GE(buf.size(), 13u + tag.size());
  EXPECT_EQ(buf[13], 9);  // tag type at the boundary librtmp will parse.
  r.Consume(static_cast<size_t>(ready));
  EXPECT_TRUE(r.buffer().empty());
}

// Multiple tags in one write: all complete tags go out, a trailing partial
// tag stays stashed and is emitted once its missing bytes arrive.
TEST(FlvRtmpReassemblerTest, CompleteTagsEmitPartialTailStashed) {
  const auto t1 = MakeTag(9, {1, 2, 3});
  const auto t2 = MakeTag(8, {4, 5});
  const auto t3 = MakeTag(9, {6, 7, 8, 9});
  FlvRtmpReassembler r;

  auto stream = Concat(MakeFlvHeader(), t1);
  stream = Concat(stream, t2);
  const size_t partial = 7;  // less than t3's 11-byte header.
  stream.insert(stream.end(), t3.begin(), t3.begin() + partial);
  const int ready = FeedAll(r, stream);
  ASSERT_EQ(ready, static_cast<int>(13 + t1.size() + t2.size()));
  EXPECT_TRUE(r.header_done());

  r.Consume(static_cast<size_t>(ready));
  EXPECT_EQ(r.buffer().size(), partial);  // only the partial t3 remains.
  const int ready2 = FeedAll(r, std::vector<uint8_t>(t3.begin() + partial,
                                                     t3.end()));
  ASSERT_EQ(ready2, static_cast<int>(t3.size()));
  r.Consume(static_cast<size_t>(ready2));
  EXPECT_TRUE(r.buffer().empty());
}

// A body whose bytes span three AVIO writes must not be flushed early.
TEST(FlvRtmpReassemblerTest, SplitBodyAcrossWrites) {
  const auto tag = MakeTag(9, std::vector<uint8_t>(40, 0xab));
  FlvRtmpReassembler r;
  ASSERT_EQ(FeedAll(r, MakeFlvHeader()), 0);

  // 4 header bytes, then the rest of the header + first 10 body bytes,
  // then the remaining body + PreviousTagSize: only the last write completes.
  for (size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(r.Feed(&tag[i], 1), 0);
  }
  EXPECT_EQ(r.Feed(tag.data() + 4, 14), 0);
  const int ready = r.Feed(tag.data() + 18, tag.size() - 18);
  ASSERT_EQ(ready, static_cast<int>(13 + tag.size()));  // header + whole tag.
}

// Non-FLV input (e.g. a misconfigured output format) fails loudly instead of
// stashing unbounded bytes and silently corrupting the stream.
TEST(FlvRtmpReassemblerTest, RejectsNonFlvHeader) {
  FlvRtmpReassembler r;
  const std::vector<uint8_t> bad = {'N', 'O', 'T', 0, 0, 0, 0, 0, 0,
                                     0,   0,   0,   0};
  EXPECT_EQ(FeedAll(r, bad), -1);
}

TEST(FlvRtmpReassemblerTest, RejectsOversizedStash) {
  FlvRtmpReassembler r;
  std::vector<uint8_t> chunk(4096, 0);
  int result = 0;
  for (size_t i = 0; i < FlvRtmpReassembler::kMaxStashBytes / chunk.size() + 2 &&
                     result >= 0;
       ++i) {
    result = FeedAll(r, chunk);
  }
  EXPECT_EQ(result, -1);
}

}  // namespace
}  // namespace xmedia::media

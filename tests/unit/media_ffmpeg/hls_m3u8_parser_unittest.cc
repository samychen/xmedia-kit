// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/hls_m3u8_parser.h"

#include "gtest/gtest.h"

namespace xmedia::media::hls {
namespace {

TEST(HlsM3U8ParserTest, RejectsABodyWithoutTheM3U8Signature) {
  auto result = ParseM3U8("<html>404 not found</html>", "http://x/p.m3u8");
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::kSourceUnsupported);
}

TEST(HlsM3U8ParserTest, RejectsAnEmptyPlaylist) {
  auto result = ParseM3U8("#EXTM3U\n", "http://x/p.m3u8");
  ASSERT_FALSE(result.has_value());
}

TEST(HlsM3U8ParserTest, ParsesAMediaPlaylistTimeline) {
  const std::string text =
      "#EXTM3U\n"
      "#EXT-X-VERSION:3\n"
      "#EXT-X-TARGETDURATION:10\n"
      "#EXT-X-MEDIA-SEQUENCE:2680\n"
      "#EXTINF:9.009,\n"
      "seg0.ts\n"
      "#EXTINF:9.009,\n"
      "seg1.ts\n"
      "#EXTINF:10.0,first\n"
      "seg2.ts\n"
      "#EXT-X-ENDLIST\n";
  auto result = ParseM3U8(text, "http://cdn/live/index.m3u8");
  ASSERT_TRUE(result.has_value()) << result.error().summary();
  const Playlist& playlist = *result;
  EXPECT_FALSE(playlist.is_master);
  EXPECT_FALSE(playlist.is_live);
  EXPECT_TRUE(playlist.has_endlist);
  EXPECT_EQ(playlist.target_duration, 10);
  EXPECT_EQ(playlist.media_sequence, 2680);
  ASSERT_EQ(playlist.segments.size(), 3u);
  EXPECT_EQ(playlist.segments[0].uri, "http://cdn/live/seg0.ts");
  EXPECT_EQ(playlist.segments[0].media_sequence, 2680);
  EXPECT_NEAR(playlist.segments[0].duration.InSecondsF(), 9.009, 1e-3);
  EXPECT_EQ(playlist.segments[1].media_sequence, 2681);
  // The timeline: each segment starts at the sum of the durations before it.
  EXPECT_NEAR(playlist.segments[1].timeline_start.InSecondsF(), 9.009, 1e-3);
  EXPECT_NEAR(playlist.segments[2].timeline_start.InSecondsF(), 18.018, 1e-3);
  EXPECT_EQ(playlist.segments[2].title, "first");
  EXPECT_NEAR(playlist.total_duration.InSecondsF(), 28.018, 1e-3);
}

TEST(HlsM3U8ParserTest, LivePlaylistHasNoEndlist) {
  auto result =
      ParseM3U8("#EXTM3U\n#EXTINF:6.0,\na.ts\n", "http://cdn/live.m3u8");
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->is_live);
  EXPECT_FALSE(result->has_endlist);
}

TEST(HlsM3U8ParserTest, AppliesKeyAndDiscontinuityToTheRightSegments) {
  const std::string text =
      "#EXTM3U\n"
      "#EXT-X-KEY:METHOD=AES-128,URI=\"https://k/"
      "1.key\",IV=0x9c7db8778570d05c3177c349fd9236aa\n"
      "#EXTINF:6.0,\nenc0.ts\n"
      "#EXTINF:6.0,\nenc1.ts\n"
      "#EXT-X-DISCONTINUITY\n"
      "#EXT-X-KEY:METHOD=NONE\n"
      "#EXTINF:6.0,\nplain2.ts\n";
  auto result = ParseM3U8(text, "http://cdn/index.m3u8");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->segments.size(), 3u);
  EXPECT_TRUE(result->segments[0].key.is_aes_128());
  EXPECT_EQ(result->segments[0].key.uri, "https://k/1.key");
  EXPECT_EQ(result->segments[0].key.iv_hex,
            "0x9c7db8778570d05c3177c349fd9236aa");
  EXPECT_TRUE(result->segments[1].key.is_aes_128());   // key persists
  EXPECT_FALSE(result->segments[2].key.is_aes_128());  // NONE cleared it
  EXPECT_TRUE(result->segments[2].key.is_none());
  EXPECT_TRUE(result->segments[2].discontinuity);
  EXPECT_FALSE(result->segments[0].discontinuity);
  EXPECT_TRUE(result->session_key.is_aes_128());
}

TEST(HlsM3U8ParserTest, ParsesAMasterPlaylistWithVariantsAndRenditions) {
  const std::string text =
      "#EXTM3U\n"
      "#EXT-X-MEDIA:TYPE=AUDIO,GROUP-ID=\"aud\",NAME=\"en\","
      "LANGUAGE=\"en\",DEFAULT=YES,AUTOSELECT=YES,URI=\"audio/en.m3u8\"\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=1280000,AVERAGE-BANDWIDTH=1000000,"
      "CODECS=\"avc1.64001f,mp4a.40.2\",RESOLUTION=1280x720,NAME=\"720p\"\n"
      "v/720.m3u8\n"
      "#EXT-X-STREAM-INF:BANDWIDTH=2560000,CODECS=\"avc1.640028\"\n"
      "v/1080.m3u8\n";
  auto result = ParseM3U8(text, "http://cdn/master.m3u8");
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->is_master);
  ASSERT_EQ(result->variants.size(), 2u);
  EXPECT_EQ(result->variants[0].bandwidth, 1280000);
  EXPECT_EQ(result->variants[0].average_bandwidth, 1000000);
  EXPECT_EQ(result->variants[0].resolution, "1280x720");
  EXPECT_EQ(result->variants[0].name, "720p");
  EXPECT_EQ(result->variants[0].uri, "http://cdn/v/720.m3u8");
  EXPECT_EQ(result->variants[1].uri, "http://cdn/v/1080.m3u8");
  ASSERT_EQ(result->renditions.size(), 1u);
  EXPECT_EQ(result->renditions[0].group_id, "aud");
  EXPECT_TRUE(result->renditions[0].is_default);
  EXPECT_EQ(result->renditions[0].uri, "http://cdn/audio/en.m3u8");
}

TEST(HlsM3U8ParserTest, RecordsLLHLSTags) {
  const std::string text =
      "#EXTM3U\n"
      "#EXT-X-PART-INF:PART-TARGET=0.5\n"
      "#EXT-X-PART:DURATION=0.5,URI=\"p0.mp4\"\n"
      "#EXT-X-PRELOAD-HINT:TYPE=PART,URI=\"p1.mp4\"\n"
      "#EXTINF:4.0,\nseg.ts\n";
  auto result = ParseM3U8(text, "http://cdn/ll.m3u8");
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->has_ll_hls_tags);
  EXPECT_TRUE(result->is_live);
  ASSERT_EQ(result->segments.size(), 1u);
}

TEST(HlsM3U8ParserTest, ParsesByteRanges) {
  const std::string text =
      "#EXTM3U\n"
      "#EXTINF:6.0,\n"
      "#EXT-X-BYTERANGE:1000@500\n"
      "byte.ts\n"
      "#EXTINF:6.0,\n"
      "#EXT-X-BYTERANGE:1000\n"
      "byte.ts\n"
      "#EXTINF:6.0,\n"
      "#EXT-X-BYTERANGE:1000\n"
      "byte.ts\n";
  auto result = ParseM3U8(text, "http://cdn/index.m3u8");
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result->segments.size(), 3u);
  EXPECT_EQ(result->segments[0].byterange_start, 500);
  EXPECT_EQ(result->segments[0].byterange_length, 1000);
  // No explicit start: continue after the previous range.
  EXPECT_EQ(result->segments[1].byterange_start, 1500);
  EXPECT_EQ(result->segments[2].byterange_start, 2500);
}

TEST(HlsM3U8ParserTest, ResolveUrlCoversTheFourWildShapes) {
  const std::string base = "http://cdn.example.com/live/hi/index.m3u8";
  EXPECT_EQ(ResolveUrl(base, "seg0.ts"),
            "http://cdn.example.com/live/hi/seg0.ts");
  EXPECT_EQ(ResolveUrl(base, "/abs/seg.ts"),
            "http://cdn.example.com/abs/seg.ts");
  EXPECT_EQ(ResolveUrl(base, "//other.example.com/s.ts"),
            "http://other.example.com/s.ts");
  EXPECT_EQ(ResolveUrl(base, "https://else.com/x.ts"), "https://else.com/x.ts");
  // Local playlist bases resolve like filesystem paths.
  EXPECT_EQ(ResolveUrl("/data/hls/index.m3u8", "seg0.ts"), "/data/hls/seg0.ts");
}

}  // namespace
}  // namespace xmedia::media::hls

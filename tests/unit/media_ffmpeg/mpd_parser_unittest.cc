// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/mpd_parser.h"

#include "gtest/gtest.h"

namespace avbase::media::dash {
namespace {

constexpr std::string_view kMpd =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
    "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"static\"\n"
    "     mediaPresentationDuration=\"PT10S\">\n"
    "  <Period>\n"
    "    <AdaptationSet mimeType=\"video/mp4\">\n"
    "      <SegmentTemplate media=\"$RepresentationID$/$Number$.m4s\"\n"
    "                       initialization=\"$RepresentationID$/init.mp4\"\n"
    "                       startNumber=\"1\" duration=\"960\"\n"
    "                       timescale=\"480\"/>\n"
    "      <Representation id=\"v720\" bandwidth=\"2000000\"\n"
    "                       codecs=\"avc1.64001f\"/>\n"
    "      <Representation id=\"v1080\" bandwidth=\"4000000\"\n"
    "                       codecs=\"avc1.640028\"/>\n"
    "    </AdaptationSet>\n"
    "  </Period>\n"
    "</MPD>\n";

TEST(MpdParserTest, ParsesRepresentationsAndSegmentTemplates) {
  auto mpd = ParseMpd(kMpd, "http://cdn/live/manifest.mpd");
  ASSERT_TRUE(mpd.has_value()) << mpd.error().summary();
  EXPECT_FALSE(mpd->is_live);
  EXPECT_EQ(mpd->media_presentation_duration, base::Seconds(10));
  ASSERT_EQ(mpd->representations.size(), 2u);
  const MpdRepresentation& v720 = mpd->representations[0];
  EXPECT_EQ(v720.id, "v720");
  EXPECT_EQ(v720.bandwidth, 2000000);
  EXPECT_EQ(v720.mime_type, "video/mp4");
  // 10 s presentation over 960/480 = 2 s segments -> 5 segments, and the
  // initialisation segment rides in front.
  ASSERT_EQ(v720.segment_urls.size(), 6u);
  EXPECT_EQ(v720.segment_urls[0], "http://cdn/live/v720/init.mp4");
  EXPECT_EQ(v720.segment_urls[1], "http://cdn/live/v720/1.m4s");
  EXPECT_EQ(v720.segment_urls[5], "http://cdn/live/v720/5.m4s");
  EXPECT_EQ(v720.segment_duration, base::Seconds(2));
  EXPECT_EQ(v720.total_duration, base::Seconds(10));
}

TEST(MpdParserTest, StartNumberOffsetsTheSequence) {
  std::string mpd_text(kMpd);
  const size_t at = mpd_text.find("startNumber=\"1\"");
  mpd_text.replace(at, 15, "startNumber=\"100\"");
  auto parsed = ParseMpd(mpd_text, "http://cdn/manifest.mpd");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->representations[0].segment_urls[1],
            "http://cdn/v720/100.m4s");
}

TEST(MpdParserTest, DynamicProfileIsLive) {
  std::string mpd_text(kMpd);
  mpd_text.replace(mpd_text.find("type=\"static\""), 14, "type=\"dynamic\"");
  auto parsed = ParseMpd(mpd_text, "http://cdn/manifest.mpd");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_TRUE(parsed->is_live);
}

TEST(MpdParserTest, RejectsBrokenXmlAndEmptyManifests) {
  auto broken = ParseMpd("<MPD><Period></MPD>", "http://x/manifest.mpd");
  ASSERT_FALSE(broken.has_value());
  auto empty =
      ParseMpd("<MPD mediaPresentationDuration=\"PT10S\"><Period/></MPD>",
               "http://x/manifest.mpd");
  ASSERT_FALSE(empty.has_value());
  EXPECT_EQ(empty.error().code(), ErrorCode::kSourceUnsupported);
}

TEST(MpdParserTest, ResolveMpdUrlCoversTheWildShapes) {
  const std::string base = "http://cdn/dash/manifest.mpd";
  EXPECT_EQ(ResolveMpdUrl(base, "v720/1.m4s"), "http://cdn/dash/v720/1.m4s");
  EXPECT_EQ(ResolveMpdUrl(base, "/abs/1.m4s"), "http://cdn/abs/1.m4s");
  EXPECT_EQ(ResolveMpdUrl(base, "https://other/1.m4s"), "https://other/1.m4s");
}

}  // namespace
}  // namespace avbase::media::dash

// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fstream>

#include "media/ffmpeg/mpd_parser.h"

#include "gtest/gtest.h"

namespace xmedia::media::dash {
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

// A dynamic manifest: no mediaPresentationDuration, so the segment list comes
// from (publishTime - availabilityStartTime) instead. This is what makes a live
// DASH source playable at all -- the parser refusing it was the registered
// follow-up.
TEST(MpdParserTest, DynamicManifestExpandsToThePublishedWindow) {
  const std::string live =
      "<MPD type=\"dynamic\" availabilityStartTime=\"2026-10-31T09:00:00Z\"\n"
      "     publishTime=\"2026-10-31T09:01:00Z\" minimumUpdatePeriod=\"PT2S\"\n"
      "     timeShiftBufferDepth=\"PT20S\">\n"
      "  <Period>\n"
      "    <AdaptationSet mimeType=\"video/mp4\">\n"
      "      <SegmentTemplate media=\"seg$Number$.m4s\" "
      "initialization=\"init.mp4\"\n"
      "                       duration=\"2\" timescale=\"1\" "
      "startNumber=\"10\"/>\n"
      "      <Representation id=\"v1\" bandwidth=\"900000\"/>\n"
      "    </AdaptationSet>\n"
      "  </Period>\n"
      "</MPD>\n";
  auto mpd = ParseMpd(live, "http://cdn/live/manifest.mpd");
  ASSERT_TRUE(mpd.has_value()) << mpd.error().summary();
  EXPECT_TRUE(mpd->is_live);
  EXPECT_EQ(mpd->availability_start_time_ms, 1'793'437'200'000LL);
  EXPECT_EQ(mpd->minimum_update_period_ms, 2000);
  EXPECT_EQ(mpd->time_shift_buffer_depth_ms, 20'000);
  ASSERT_EQ(mpd->representations.size(), 1u);
  const MpdRepresentation& rep = mpd->representations[0];
  EXPECT_EQ(rep.segment_duration, base::Seconds(2));
  // 60 s published over 2 s segments = 30 segments, but the 20 s window keeps
  // the last 11 (the one being produced plus 10 behind it), renumbered from
  // what the grid says rather than from 0.
  EXPECT_EQ(rep.segment_urls.front(), "http://cdn/live/init.mp4");
  EXPECT_EQ(rep.segment_urls.back(),
            "http://cdn/live/seg41.m4s");   // startNumber 10 + 30 + 1
  EXPECT_EQ(rep.segment_urls.size(), 12u);  // init + 11 window segments
}

// A dynamic manifest missing the anchor is a manifest defect, and the parser
// says so by expanding nothing rather than inventing an epoch.
TEST(MpdParserTest, DynamicManifestWithoutAnAnchorExpandsNothing) {
  const std::string live =
      "<MPD type=\"dynamic\">\n"
      "  <Period>\n"
      "    <AdaptationSet mimeType=\"video/mp4\">\n"
      "      <SegmentTemplate media=\"seg$Number$.m4s\" duration=\"2\"\n"
      "                       timescale=\"1\" startNumber=\"1\"/>\n"
      "      <Representation id=\"v1\" bandwidth=\"900000\"/>\n"
      "    </AdaptationSet>\n"
      "  </Period>\n"
      "</MPD>\n";
  auto mpd = ParseMpd(live, "http://cdn/live/manifest.mpd");
  // A parse FAILURE, not an empty result: a manifest naming no locatable
  // segment is unusable, and the error says which of the two reasons it is.
  ASSERT_FALSE(mpd.has_value());
  EXPECT_EQ(mpd.error().code(), ErrorCode::kSourceUnsupported);
  EXPECT_NE(mpd.error().summary().find("timing anchor"), std::string::npos)
      << mpd.error().summary();
}

TEST(MpdParserTest, ParsesTheTsFixtureManifest) {
  std::ifstream in(std::string(XMEDIA_TESTDATA_DIR) + "/dash/local.mpd");
  ASSERT_TRUE(in.good());
  std::string body((std::istreambuf_iterator<char>(in)),
                   std::istreambuf_iterator<char>());
  auto mpd = ParseMpd(body, "file://" XMEDIA_TESTDATA_DIR "/dash/local.mpd");
  ASSERT_TRUE(mpd.has_value()) << mpd.error().summary();
  ASSERT_EQ(mpd->representations.size(), 1u);
  EXPECT_EQ(mpd->representations[0].mime_type, "video/mp2t");
  EXPECT_EQ(mpd->representations[0].segment_urls.size(), 2u);
  EXPECT_EQ(mpd->representations[0].segment_duration, base::Seconds(1));
  EXPECT_EQ(mpd->representations[0].total_duration, base::Seconds(2));
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
}  // namespace xmedia::media::dash

// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/external_text_track.h"

#include "gtest/gtest.h"

namespace avbase::media {
namespace {

constexpr std::string_view kSrt =
    "1\n"
    "00:00:01,000 --> 00:00:02,000\n"
    "Hello <i>world</i>\n"
    "\n"
    "2\n"
    "00:00:02,500 --> 00:00:04,000\n"
    "Second line one\nsecond line two\n"
    "\n";

constexpr std::string_view kVtt =
    "WEBVTT\n"
    "\n"
    "intro-cue\n"
    "00:01.000 --> 00:02.000\n"
    "Hello <b>vtt</b>\n"
    "\n"
    "00:00:02.500 --> 00:00:04.000\n"
    "Hours form\n"
    "\n";

TEST(SrtParserTest, ParsesCuesWithTitlesAndTagsStripped) {
  auto cues = ParseSrt(kSrt, /*source_id=*/7);
  ASSERT_TRUE(cues.has_value()) << cues.error().summary();
  ASSERT_EQ(cues->size(), 2u);
  EXPECT_EQ((*cues)[0].source_id, 7);
  EXPECT_EQ((*cues)[0].pts, base::Seconds(1));
  EXPECT_EQ((*cues)[0].duration, base::Seconds(1));
  EXPECT_EQ((*cues)[0].text, "Hello world");
  EXPECT_EQ((*cues)[1].text, "Second line one\nsecond line two");
  EXPECT_EQ((*cues)[1].duration, base::Milliseconds(1500));
}

TEST(SrtParserTest, RejectsAMalformedTimestamp) {
  auto cues = ParseSrt("1\n00:01 --> 00:02\nhi\n\n", 0);
  ASSERT_FALSE(cues.has_value());
  EXPECT_EQ(cues.error().code(), ErrorCode::kSourceUnsupported);
}

TEST(WebVttParserTest, ParsesBothTimestampShapes) {
  auto cues = ParseWebVtt(kVtt, /*source_id=*/3);
  ASSERT_TRUE(cues.has_value()) << cues.error().summary();
  ASSERT_EQ(cues->size(), 2u);
  EXPECT_EQ((*cues)[0].pts, base::Seconds(1));
  EXPECT_EQ((*cues)[0].text, "Hello vtt");
  EXPECT_EQ((*cues)[1].pts, base::Seconds(2) + base::Milliseconds(500));
}

TEST(WebVttParserTest, RejectsAMissingSignature) {
  auto cues = ParseWebVtt("00:01.000 --> 00:02.000\nhi\n\n", 0);
  ASSERT_FALSE(cues.has_value());
}

TEST(ExternalTextTrackTest, FormatIsDetectedAndCuesScheduled) {
  auto track = ExternalTextTrack::Parse(kVtt, "subs.vtt", 5);
  ASSERT_TRUE(track.has_value());
  EXPECT_EQ(track->track_id(), 5);
  EXPECT_EQ(track->cue_count(), 2u);

  // Window (0, 1.5]: the first cue (1.0-2.0) is due once.
  auto due = track->CuesBetween(base::TimeDelta(), base::Milliseconds(1500));
  ASSERT_EQ(due.size(), 1u);
  EXPECT_EQ(due[0].text, "Hello vtt");
  // Window (2.6, 3.0]: the hours-form cue only.
  due = track->CuesBetween(base::Milliseconds(2600), base::Milliseconds(3000));
  ASSERT_EQ(due.size(), 1u);
  EXPECT_EQ(due[0].text, "Hours form");
  // A clock that did not move: nothing new.
  due = track->CuesBetween(base::Milliseconds(3000), base::Milliseconds(3000));
  EXPECT_TRUE(due.empty());
}

TEST(ExternalTextTrackTest, DelayShiftsTheWindowAndSelectionGates) {
  auto track = ExternalTextTrack::Parse(kSrt, "subs.srt", 1);
  ASSERT_TRUE(track.has_value());
  // The cue sits at 1.0-2.0; a +500ms delay moves it to 1.5-2.5.
  track->SetDelay(base::Milliseconds(500));
  auto early = track->CuesBetween(base::TimeDelta(), base::Milliseconds(1200));
  EXPECT_TRUE(early.empty());
  auto late =
      track->CuesBetween(base::Milliseconds(1600), base::Milliseconds(1800));
  ASSERT_EQ(late.size(), 1u);
  // The reported cue keeps its ORIGINAL timestamps (the renderer formats
  // them); only the delivery window shifted.
  EXPECT_EQ(late[0].pts, base::Seconds(1));

  track->SetSelected(false);
  EXPECT_TRUE(track->CuesBetween(base::TimeDelta(), base::Seconds(5)).empty());
}

TEST(ExternalTextTrackTest, BomIsToleratedBeforeTheSignature) {
  std::string with_bom = "\xEF\xBB\xBF";
  with_bom += kVtt;
  auto track = ExternalTextTrack::Parse(with_bom, "subs.vtt", 2);
  ASSERT_TRUE(track.has_value()) << track.error().summary();
  EXPECT_EQ(track->cue_count(), 2u);
}

}  // namespace
}  // namespace avbase::media

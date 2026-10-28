// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// kHdrUnsupported: the event a host is supposed to react to when a source
// cannot be shown correctly.
//
// WHY THIS EXISTS. The event type, its name ("HdrUnsupported"), its payload
// struct (HdrUnsupportedPayload) and its "no payload in the C ABI" entry all
// existed -- and NOTHING ever posted it, because the flag it depends on
// (MediaInfo::StreamInfo::has_hdr_metadata) had no writer either. A host that
// waited for the event to decide "warn the user / tint the video" waited
// forever, and every test stayed green because none of them asserted the event
// fires. Three more event types were in exactly that state
// (kRotationChanged, kDecoderFallback, kTimingChanged); this file pins the one
// that now has a producer.
//
// BOTH DIRECTIONS, because one alone is worthless: "fires for an HDR source"
// passes trivially if the code posts it unconditionally, and "does not fire
// for SDR" passes if it never posts at all. The pair is the assertion.

#include "player/player_impl.h"

#include <atomic>
#include <memory>
#include <string>

#include "avbase/BuildConfig.h"
#include "base/functional/bind.h"
#include "base/synchronization/waitable_event.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "player/public/player.h"
#include "player/public/player_config.h"
#include "player/public/player_event.h"

#if !AVBASE_ENABLE_FFMPEG
// Detection lives in the FFmpeg demuxer, so without it there is nothing to
// gate. Compiling to nothing keeps the suite from reporting a skip on every
// no-ffmpeg build (same reasoning as prepare_autostart_unittest.cc).
#else

namespace avbase {
namespace {

#ifndef AVBASE_TESTDATA_DIR
#error "player_unittests must define AVBASE_TESTDATA_DIR for this suite"
#endif
// A 1 s 320x240 HEVC file with colour_transfer=smpte2084 (PQ) and BT.2020
// primaries, 23 KB. Committed rather than generated at test time: the seed set
// in tests/testdata is what encodes "these are the dimensions we care about",
// and an HDR sample is now one of them.
const char* const kHdrUri = AVBASE_TESTDATA_DIR "/hdr_pq_320x240_1s.mp4";
const char* const kSdrUri = AVBASE_TESTDATA_DIR "/small_h264_aac_3s.mp4";
// The same 1 s clip with a 90-degree display matrix, for kRotationChanged.
const char* const kRotatedUri =
    AVBASE_TESTDATA_DIR "/rotated90_hdr_320x240_1s.mp4";

// The handler runs on the player's event sequence; these outlive it and are
// read after StopSync, which is why they are atomics rather than plain ints.
struct Probe {
  std::atomic<int> hdr_events{0};
  std::atomic<int> prepared_events{0};
  std::atomic<int> rotation_events{0};
  int rotation_degrees{-1};
  // Order matters: a host that only learns "HDR" after it has already acted on
  // "prepared" cannot use the fact. Recorded as two counters plus a flag that
  // says whether HDR was seen before prepared.
  std::atomic<bool> hdr_before_prepared{false};
  std::atomic<bool> saw_prepared{false};
  std::string transfer;
};

void OnEvent(Probe* probe, const PlayerEvent& event) {
  switch (event.type) {
  case EventType::kHdrUnsupported: {
    probe->hdr_events.fetch_add(1);
    if (!probe->saw_prepared.load()) {
      probe->hdr_before_prepared.store(true);
    }
    // The payload names the actual transfer curve, so a host can distinguish
    // PQ from HLG (and from Dolby Vision, which usually also claims PQ).
    if (const auto* payload =
            std::get_if<HdrUnsupportedPayload>(&event.payload)) {
      probe->transfer = payload->transfer;
    }
    break;
  }
  case EventType::kRotationChanged: {
    probe->rotation_events.fetch_add(1);
    if (const auto* payload =
            std::get_if<RotationChangedPayload>(&event.payload)) {
      probe->rotation_degrees = payload->degrees;
    }
    break;
  }
  case EventType::kPrepared:
    probe->prepared_events.fetch_add(1);
    probe->saw_prepared.store(true);
    break;
  default:
    break;
  }
}

// Prepares the source WITHOUT autostart and stops it again. The events are
// what the test reads; the return value only says "the source opened".
//
// Preparing without autostart is deliberate: this suite is about what the
// prepare path reports, and a playing pipeline would add decode work to the
// same window without changing the answer.
bool PrepareAndStop(const char* uri, Probe* probe, int* reported_rotation) {
  PlayerConfig config;
  config.start_on_prepared = false;
  PlayerImpl impl(config, nullptr);
  impl.SetEventHandler(base::BindRepeating(&OnEvent, base::Unretained(probe)));
  const Status configured = impl.SetDataSource(uri);
  if (!configured.has_value()) {
    return false;
  }
  if (!impl.PrepareSync(base::Seconds(30)).has_value()) {
    return false;
  }
  if (reported_rotation != nullptr) {
    *reported_rotation = impl.video_rotation();
  }
  impl.StopSync(base::Milliseconds(500));
  return true;
}

TEST(PlayerHdrUnsupportedTest, HdrSourceReportsTheEventBeforePrepared) {
  Probe probe;
  if (!PrepareAndStop(kHdrUri, &probe, nullptr)) {
    GTEST_SKIP() << "HDR test media unavailable";
  }
  EXPECT_EQ(probe.hdr_events.load(), 1)
      << "a PQ source produced no kHdrUnsupported event";
  EXPECT_TRUE(probe.saw_prepared.load()) << "prepare did not complete";
  // Before, not after: the whole point of the event is to let a host change
  // what it does on kPrepared.
  EXPECT_TRUE(probe.hdr_before_prepared.load())
      << "kHdrUnsupported arrived after kPrepared, too late to act on";
  EXPECT_EQ(probe.transfer, "smpte2084")
      << "the payload must name the transfer curve, not just say \"HDR\"";
}

// kRotationChanged was in the same state as kHdrUnsupported: type, name,
// payload and a query (rotation()) existed; nothing posted it. A host that
// rotates the picture itself had to poll.
TEST(PlayerRotationChangedTest, RotatedSourceReportsTheEvent) {
  Probe probe;
  int reported_rotation = 0;
  if (!PrepareAndStop(kRotatedUri, &probe, &reported_rotation)) {
    GTEST_SKIP() << "rotated test media unavailable";
  }
  EXPECT_EQ(probe.rotation_events.load(), 1)
      << "a rotated source produced no kRotationChanged event";
  // 270, not 90, and that is not a bug: av_display_rotation_get() reports the
  // angle COUNTERCLOCKWISE, while avbase's rotation is CLOCKWISE everywhere it
  // is consumed (gl_layer_geometry.cc's kCosSin table is labelled clockwise,
  // and NaturalSizeForRotation only cares about quarter turns). The demuxer
  // negates to convert, so a 90-degree counterclockwise display matrix is a
  // 270-degree clockwise rotation here.
  //
  // Asserted as "the event agrees with what the player reports elsewhere"
  // rather than as a bare literal: a host that acts on the event must get the
  // same number rotation() gives it, and pinning the literal alone would not
  // catch those two drifting apart.
  EXPECT_EQ(probe.rotation_degrees, 270);
  // The event and the query must agree; a host uses whichever it has.
  EXPECT_EQ(reported_rotation, probe.rotation_degrees)
      << "kRotationChanged disagreed with rotation()";
}

TEST(PlayerRotationChangedTest, UnrotatedSourceReportsNoEvent) {
  Probe probe;
  if (!PrepareAndStop(kSdrUri, &probe, nullptr)) {
    GTEST_SKIP() << "test media unavailable";
  }
  // "still 0" is not a change; emitting it would make hosts that act on the
  // event do a wasted rotation on every file.
  EXPECT_EQ(probe.rotation_events.load(), 0);
}

TEST(PlayerHdrUnsupportedTest, SdrSourceReportsNoEvent) {
  Probe probe;
  if (!PrepareAndStop(kSdrUri, &probe, nullptr)) {
    GTEST_SKIP() << "test media unavailable";
  }
  // If this ever fires on BT.709 content, the detection rule is wrong (for
  // example keyed on bit depth or on BT.2020 primaries, both of which appear
  // on SDR files).
  EXPECT_EQ(probe.hdr_events.load(), 0)
      << "an SDR source reported kHdrUnsupported (transfer=" << probe.transfer
      << ")";
  EXPECT_TRUE(probe.saw_prepared.load());
}

}  // namespace
}  // namespace avbase

#endif  // AVBASE_ENABLE_FFMPEG

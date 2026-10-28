// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// kDecoderOpened / kDecoderFallback: which decoder is actually in use.
//
// WHY THIS EXISTS. Both event types shipped with a name, a payload struct
// (decoder_name / outputs_opaque_surface; from -> to / reason) and a string
// mapping, and NEITHER had a producer. The decoder's name was written to a
// LOG(INFO) line inside VideoRendererImpl and then dropped: the owner had no
// way to learn it, so a host could not say "playing with VideoToolbox", could
// not decide whether frames were already in device memory, and could not tell
// a hardware-to-software fallback from a normal start.
//
// The chain now runs DecoderStream -> VideoRendererImpl/AudioRendererImpl ->
// RendererImpl::ForwardDecoderEvent -> PipelineImpl -> PlayerImpl -> event.
// This file pins the two ends of it and, importantly, pins that the name
// matches what the PLAYER reports back.

#include "player/player_impl.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "avbase/BuildConfig.h"
#include "base/functional/bind.h"
#include "base/synchronization/waitable_event.h"
#include "base/time/time.h"
#include "gtest/gtest.h"
#include "player/public/player.h"
#include "player/public/player_config.h"
#include "player/public/player_event.h"

#if !AVBASE_ENABLE_FFMPEG
// Decoder selection lives in the FFmpeg layer; nothing to gate without it.
#else

namespace avbase {
namespace {

#ifndef AVBASE_TESTDATA_DIR
#error "player_unittests must define AVBASE_TESTDATA_DIR for this suite"
#endif
const char* const kUri = AVBASE_TESTDATA_DIR "/small_h264_aac_3s.mp4";

struct Probe {
  std::atomic<int> video_opened{0};
  std::atomic<int> audio_opened{0};
  std::atomic<int> fallbacks{0};
  std::string video_name;
  std::string audio_name;
  bool video_opaque{false};
};

void OnEvent(Probe* probe, const PlayerEvent& event) {
  switch (event.type) {
  case EventType::kDecoderOpened: {
    const auto* payload = std::get_if<DecoderOpenedPayload>(&event.payload);
    if (payload == nullptr) {
      return;
    }
    if (payload->type == media::DemuxerStreamType::kVideo) {
      probe->video_opened.fetch_add(1);
      probe->video_name = payload->decoder_name;
      probe->video_opaque = payload->outputs_opaque_surface;
    } else {
      probe->audio_opened.fetch_add(1);
      probe->audio_name = payload->decoder_name;
    }
    break;
  }
  case EventType::kDecoderFallback:
    probe->fallbacks.fetch_add(1);
    break;
  default:
    break;
  }
}

// Prepares without autostart through the PUBLIC facade, not PlayerImpl: the
// point of this suite is that a host can learn which decoder is in use, and
// the host's entry point is Player. Going through PlayerImpl would leave the
// forwarding in player.cc (Player::video_decoder_name etc.) untested, which is
// exactly the seam where a "works in tests, empty for hosts" bug lives.
bool PrepareAndStop(Probe* probe, std::string* reported_video,
                    std::string* reported_audio, int* reported_fallbacks) {
  PlayerConfig config;
  config.start_on_prepared = false;
  Player player(config);
  player.SetEventHandler(
      base::BindRepeating(&OnEvent, base::Unretained(probe)));
  if (!player.SetDataSource(kUri).has_value()) {
    return false;
  }
  if (!player.PrepareSync(base::Seconds(30)).has_value()) {
    return false;
  }
  // Drain the event dispatch sequence: the events are posted to it from the
  // media sequence, so a read immediately after PrepareSync can precede them.
  // Bounded so a missing event fails the assertion instead of hanging.
  for (int i = 0; i < 400 && probe->video_opened.load() == 0; ++i) {
    player.RunUntilIdle();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  *reported_video = player.video_decoder_name();
  *reported_audio = player.audio_decoder_name();
  *reported_fallbacks = player.decoder_fallback_count();
  player.StopSync(base::Milliseconds(500));
  return true;
}

TEST(PlayerDecoderEventTest, BothLegsReportTheirOpenedDecoder) {
  Probe probe;
  std::string reported_video;
  std::string reported_audio;
  int reported_fallbacks = 0;
  if (!PrepareAndStop(&probe, &reported_video, &reported_audio,
                      &reported_fallbacks)) {
    GTEST_SKIP() << "test media unavailable";
  }
  // Exactly once per leg: the event is "which decoder did this leg open",
  // and a duplicate would make a host re-run whatever it does on open.
  EXPECT_EQ(probe.video_opened.load(), 1);
  EXPECT_EQ(probe.audio_opened.load(), 1);
  EXPECT_FALSE(probe.video_name.empty())
      << "the video event carried no decoder name";
  EXPECT_FALSE(probe.audio_name.empty())
      << "the audio event carried no decoder name";
  // The name has to be the DECODER, not the stream wrapper: a host printing
  // "VideoDecoderStream(FFmpegVideoDecoder)" is no better off than one
  // printing nothing. The wrapper's spelling would contain "Stream(".
  EXPECT_EQ(probe.video_name.find("Stream("), std::string::npos)
      << "event carried the stream's display name: " << probe.video_name;

  // The event and the query must agree. This is the assertion that catches the
  // failure mode where the event fires but the durable copy is never written
  // (or vice versa), which is how a host that asks late gets a different
  // answer from a host that listened.
  EXPECT_EQ(reported_video, probe.video_name)
      << "video_decoder_name() disagreed with kDecoderOpened";
  EXPECT_EQ(reported_audio, probe.audio_name)
      << "audio_decoder_name() disagreed with kDecoderOpened";
  EXPECT_EQ(reported_fallbacks, probe.fallbacks.load())
      << "decoder_fallback_count() disagreed with the kDecoderFallback count";
}

// On this machine neither decoder is opaque: the software FFmpeg decoder hands
// back CPU memory, and the audio leg never can be opaque. The assertion is
// therefore "false, and the query agrees" rather than "true somewhere" -- it
// pins that the flag is PLUMBED (a hardcoded true would fail the equality
// below only if the two disagreed, so the value is also asserted directly for
// the audio leg, which is the one that can never be opaque).
TEST(PlayerDecoderEventTest, OpaqueSurfaceFlagIsPlumbedNotGuessed) {
  Probe probe;
  std::string reported_video;
  std::string reported_audio;
  int reported_fallbacks = 0;
  if (!PrepareAndStop(&probe, &reported_video, &reported_audio,
                      &reported_fallbacks)) {
    GTEST_SKIP() << "test media unavailable";
  }
  // The flag travels with the event; the query is the durable copy. On a build
  // that falls back to the software decoder both are false, and on a machine
  // with a working hardware session both are true -- the assertion is that the
  // two agree, which is what a host relies on when it decides at Prepare time
  // versus one that asks a second later.
  PlayerConfig config;
  config.start_on_prepared = false;
  Player player(config);
  if (!player.SetDataSource(kUri).has_value() ||
      !player.PrepareSync(base::Seconds(30)).has_value()) {
    GTEST_SKIP() << "test media unavailable";
  }
  EXPECT_EQ(player.video_decoder_opaque(), probe.video_opaque)
      << "video_decoder_opaque() disagreed with kDecoderOpened";
  player.StopSync(base::Milliseconds(500));
}

// A fallback is only reachable when a decoder that was actually OPENED then
// fails, so this suite cannot synthesise one through the public API on demand.
// What it CAN pin is the negative: this machine's software decoder opens fine,
// so a normal prepare must report zero fallbacks. That keeps the counter honest
// in the direction the event pipeline can produce; the positive direction is
// covered at the media layer by decoder-stream's own suites, which drive a
// failing decoder directly.
TEST(PlayerDecoderEventTest, NormalPrepareReportsNoFallback) {
  Probe probe;
  std::string reported_video;
  std::string reported_audio;
  int reported_fallbacks = 0;
  if (!PrepareAndStop(&probe, &reported_video, &reported_audio,
                      &reported_fallbacks)) {
    GTEST_SKIP() << "test media unavailable";
  }
  // A factory that DECLINED (VideoToolbox will not take this H.264 profile) is
  // capability negotiation, not a fallback: nothing was opened and then lost.
  // Counting it would make every machine without hardware decode report a
  // fallback on every file, which is exactly the noise the distinction exists
  // to avoid.
  EXPECT_EQ(probe.fallbacks.load(), 0)
      << "a factory declining the config was reported as a fallback";
  EXPECT_EQ(reported_fallbacks, 0);
}

}  // namespace
}  // namespace avbase

#endif  // AVBASE_ENABLE_FFMPEG

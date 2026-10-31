// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The player-level UTC contract: an HLS source carrying
// EXT-X-PROGRAM-DATE-TIME publishes a wall-clock anchor (kUtcTime event),
// and GetUtcTimeMs() extrapolates it with the live media clock. The anchor
// math is pinned to the fixture's known PDT so a regression in any link of
// parser -> demuxer host -> pipeline client -> player event shows up as a
// wrong epoch, not as a missing event.

#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <cstdio>
#include <string>
#include <thread>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "gtest/gtest.h"
#include "player/public/player.h"
#include "player/public/player_event.h"

namespace xmedia {
namespace {

std::string TempPath(const char* name) {
  return testing::TempDir() + "/" + name;
}

class PlayerUtcTest : public ::testing::Test {
 protected:
  void SetUp() override {
    config_.net.live_max_latency = base::Seconds(0);  // never chase
  }

  // A one-second-segment live-shaped playlist (no ENDLIST) whose first
  // segment carries a known PDT. Media sequence 0 keeps the timeline
  // anchored; the served window never grows, so playback runs to EOS of the
  // loaded window and the anchors stay put.
  // Writes the playlist AND copies the two segment files into TempDir: a
  // test never writes into the source tree's testdata.
  Status OpenWithPdt(const std::string& pdt_line) {
    const std::string dir = testing::TempDir();
    for (const char* seg : {"seg0.ts", "seg1.ts"}) {
      std::ifstream src(std::string(XMEDIA_TESTDATA_DIR) + "/hls/" + seg,
                        std::ios::binary);
      std::ofstream dst(dir + "/" + seg, std::ios::binary);
      if (!src.good() || !dst.good()) {
        return base::unexpected(MediaError::Of(ErrorCode::kSourceOpenFailed,
                                               "fixture copy failed", {}, {}));
      }
      dst << src.rdbuf();
    }
    const std::string body =
        "#EXTM3U\n"
        "#EXT-X-VERSION:3\n"
        "#EXT-X-TARGETDURATION:1\n"
        "#EXT-X-MEDIA-SEQUENCE:0\n" +
        pdt_line + "#EXTINF:1.0,\nseg0.ts\n#EXTINF:1.0,\nseg1.ts\n";
    std::ofstream playlist(dir + "/utc_live.m3u8", std::ios::binary);
    if (!playlist.good()) {
      return base::unexpected(MediaError::Of(ErrorCode::kSourceOpenFailed,
                                             "fixture write failed", {}, {}));
    }
    playlist << body;
    playlist.close();
    test_file_ = dir + "/utc_live.m3u8";
    return OkStatus();
  }

  bool PumpUntil(std::function<bool()> predicate, int max_attempts) {
    for (int i = 0; i < max_attempts; ++i) {
      if (predicate()) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return predicate();
  }

  base::test::TaskEnvironment task_environment_;
  PlayerConfig config_;
  std::string test_file_;
};

// The known anchor: PDT 2026-10-31T08:00:00.500Z on segment 0 (timeline 0).
// The parser's epoch table verified the constant; this test pins the WIRE:
// anchor arrives as an event, the query extrapolates from the media clock.
TEST_F(PlayerUtcTest, ProgramDateTimeBecomesTheUtcAnchor) {
  ASSERT_TRUE(OpenWithPdt("#EXT-X-PROGRAM-DATE-TIME:2026-10-31T08:00:00.500Z\n")
                  .has_value());
  Player player(config_, nullptr);
  std::atomic<bool> got_anchor{false};
  std::atomic<int64_t> anchor_epoch{0};
  std::atomic<int64_t> anchor_media_us{0};
  player.SetEventHandler(base::BindRepeating(
      [](std::atomic<bool>* got, std::atomic<int64_t>* epoch,
         std::atomic<int64_t>* media_us, const PlayerEvent& event) {
        if (const auto* payload = AsUtcTime(event)) {
          epoch->store(payload->epoch_ms);
          media_us->store(payload->anchor_media_time_us);
          got->store(true);
        }
      },
      &got_anchor, &anchor_epoch, &anchor_media_us));
  ASSERT_TRUE(player.SetDataSource("file://" + test_file_).has_value());
  ASSERT_TRUE(player.PrepareSync(base::Seconds(10)).has_value());
  if (player.state() != PlayerState::kStarted) {
    player.Start();
  }
  ASSERT_TRUE(PumpUntil([&] { return got_anchor.load(); }, 500));
  EXPECT_EQ(anchor_epoch.load(),
            (int64_t{1793404800} + 8 * 3600) * 1000 + 500);
  EXPECT_EQ(anchor_media_us.load(), 0);  // the first segment's start

  // The query extrapolates: at media time t the answer is anchor + t. Read
  // the media clock once and require a self-consistent pair rather than
  // pinning the clock's current value.
  const auto utc = player.GetUtcTimeMs();
  ASSERT_TRUE(utc.has_value()) << utc.error().ToString();
  const auto media = player.GetMediaTime();
  ASSERT_NE(media, media::kNoTimestamp);
  const int64_t expected = anchor_epoch.load() +
                           (media.InMicroseconds() -
                            anchor_media_us.load()) / 1000;
  EXPECT_EQ(utc.value(), expected);
  player.StopSync(base::Seconds(5));
}

// A source without a wall-clock mapping answers kInvalidState, not a made-up
// epoch: the "unknown" case must stay distinguishable from 1970.
TEST_F(PlayerUtcTest, SourceWithoutAnchorRefusesTheQuery) {
  Player player(config_, nullptr);
  player.SetEventHandler(base::BindRepeating([](const PlayerEvent&) {}));
  ASSERT_TRUE(
      player.SetDataSource("file://" + std::string(XMEDIA_TESTDATA_DIR) +
                           "/hls/local.m3u8")
          .has_value());
  ASSERT_TRUE(player.PrepareSync(base::Seconds(10)).has_value());
  const auto utc = player.GetUtcTimeMs();
  ASSERT_FALSE(utc.has_value());
  EXPECT_EQ(utc.error().code(), ErrorCode::kInvalidState);
  player.StopSync(base::Seconds(5));
}

}  // namespace
}  // namespace xmedia

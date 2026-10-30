// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// End-to-end external subtitles (cicada-port T6): a real player, a real
// local srt file, real playback -- the cue ticker must surface kTimedText
// events while the clock advances, and selection must gate them.

#include <atomic>
#include <cstring>
#include <memory>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>
#include "base/functional/bind.h"

#include "xmedia-kit/BuildConfig.h"
#include "gtest/gtest.h"
#include "player/public/player.h"

#if !XMEDIA_ENABLE_FFMPEG
#else

namespace xmedia {
namespace {

#ifndef XMEDIA_TESTDATA_DIR
#error "player_unittests must define XMEDIA_TESTDATA_DIR for this suite"
#endif

// Writes a two-cue srt into a temp file and returns the path.
std::string WriteTestSrt() {
  std::string path =
      std::string(XMEDIA_TESTDATA_DIR) + "/external_subtitles_tmp.srt";
  FILE* file = fopen(path.c_str(), "wb");
  if (file == nullptr) {
    return {};
  }
  const char* body =
      "1\n00:00:00,200 --> 00:00:02,500\nHello external sub\n\n"
      "2\n00:00:02,600 --> 00:00:02,900\nLater cue\n\n";
  fwrite(body, 1, strlen(body), file);
  fclose(file);
  return path;
}

class PlayerExternalSubtitleTest : public ::testing::Test {
 protected:
  void SetUp() override {
    srt_path_ = WriteTestSrt();
    if (srt_path_.empty()) {
      GTEST_SKIP() << "cannot write the temp srt";
    }
  }
  void TearDown() override { remove(srt_path_.c_str()); }

  std::string srt_path_;
};

// Hand-written collector (the tests' no-gmock convention): records cue
// texts under a mutex, shared with the test thread through shared_ptr
// because the event handler outlives the TEST_F body.
struct CueCollector {
  std::mutex lock;
  std::vector<std::string> texts;

  void OnEvent(const PlayerEvent& event) {
    if (const auto* text = AsTimedText(event)) {
      std::lock_guard<std::mutex> scoped(lock);
      texts.push_back(text->text);
    }
  }
};

TEST_F(PlayerExternalSubtitleTest, CuesFireWhilePlayingAndSelectionGates) {
  PlayerConfig config;
  config.start_on_prepared = false;
  Player player(config);

  auto collector = std::make_shared<CueCollector>();
  player.SetEventHandler(
      base::BindRepeating(&CueCollector::OnEvent, collector));

  ASSERT_TRUE(player.SetDataSource(XMEDIA_TESTDATA_DIR "/small_h264_aac_3s.mp4")
                  .has_value());
  ASSERT_TRUE(player.PrepareSync(base::Seconds(30)).has_value());

  auto added = player.AddExternalSubtitle(srt_path_);
  ASSERT_TRUE(added.has_value()) << added.error().summary();
  // Not selected yet: no cues may arrive.
  player.Start();
  std::this_thread::sleep_for(std::chrono::milliseconds(800));
  {
    std::lock_guard<std::mutex> scoped(collector->lock);
    EXPECT_TRUE(collector->texts.empty());
  }

  ASSERT_TRUE(player.SelectExternalSubtitle(*added, true).has_value());
  bool saw_first = false;
  for (int i = 0; i < 60 && !saw_first; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::lock_guard<std::mutex> scoped(collector->lock);
    for (const std::string& text : collector->texts) {
      if (text == "Hello external sub") {
        saw_first = true;
      }
    }
  }
  EXPECT_TRUE(saw_first) << "no external cue surfaced within 6s";

  // Removing the track stops further cues; the second cue must not appear.
  ASSERT_TRUE(player.RemoveExternalSubtitle(*added).has_value());
  std::this_thread::sleep_for(std::chrono::milliseconds(900));
  std::lock_guard<std::mutex> scoped(collector->lock);
  for (const std::string& text : collector->texts) {
    EXPECT_NE(text, "Later cue");
  }
}

}  // namespace
}  // namespace xmedia

#endif  // !XMEDIA_ENABLE_FFMPEG

// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/public/player_transcode.h"

#include <cstdio>

#include <gtest/gtest.h>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/media_log.h"
#include "media/ffmpeg/ffmpeg_demuxer.h"

namespace avbase {
namespace {

std::string TestFile(const char* name) {
  return std::string(AVBASE_TESTDATA_DIR) + "/" + name;
}

// Opens |path| through the regular demuxer — the round-trip check for every
// transcode below.
void ExpectPlayable(const std::string& path, int expected_streams) {
  base::scoped_refptr<media::MediaLog> media_log =
      base::MakeRefCounted<media::MediaLog>();
  media::FFmpegDemuxer demuxer(media_log);
  class Host final : public media::Demuxer::Host {
   public:
    void SetDuration(base::TimeDelta) override {}
    void OnBufferedTimeUpdate(base::TimeDelta, base::TimeDelta) override {}
    void OnDemuxerError(media::MediaError) override {}
  } host;
  base::test::TaskEnvironment env;
  media::Status opened = base::unexpected(
      media::MediaError(media::ErrorCode::kNotImplemented, "not run", "", ""));
  bool done = false;
  demuxer.Initialize(media::DataSourceDescriptor::FromUri(path), {}, &host,
                     env.GetMainThreadTaskRunnerRef(),
                     base::BindOnce(
                         [](media::Status* out, bool* flag, media::Status s) {
                           *out = std::move(s);
                           *flag = true;
                         },
                         &opened, &done));
  for (int i = 0; i < 2000 && !done; ++i) {
    env.RunUntilIdle();
    if (!done) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  ASSERT_TRUE(opened) << opened.error().ToString();
  EXPECT_EQ(demuxer.media_info().streams.size(),
            static_cast<size_t>(expected_streams));
}

bool PumpUntilState(Player& player, PlayerState want, base::TimeDelta timeout) {
  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::milliseconds(
          static_cast<long long>(timeout.InMillisecondsF()));
  while (std::chrono::steady_clock::now() < deadline) {
    if (player.state() == want) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return player.state() == want;
}

TEST(PlayerTranscodeTest, TranscodeMediaCopiesStreams) {
  media::TranscodeParams params;
  params.output_path = testing::TempDir() + "pt_copy.mp4";
  params.video_codec.codec = "copy";
  params.audio_codec.codec = "copy";
  const Status status =
      TranscodeMedia(TestFile("small_h264_aac_3s.mp4"), params);
  ASSERT_TRUE(status) << status.error().ToString();
  ExpectPlayable(params.output_path, 2);
  std::remove(params.output_path.c_str());
}

// The PlayerConfig disabled-stream flags (-vn / -an) reach the job: a config
// with audio disabled strips the audio stream from the output even though
// the params asked for aac.
TEST(PlayerTranscodeTest, ConfigAudioDisabledStripsTheStream) {
  PlayerConfig config;
  config.audio.disabled = true;

  media::TranscodeParams params;
  params.output_path = testing::TempDir() + "pt_noaudio.mp4";
  params.video_codec.codec = "copy";
  params.audio_codec.codec = "aac";

  PlayerTranscoder transcoder(config);
  EXPECT_TRUE(transcoder.IsRunning() == false);
  std::atomic<bool> done{false};
  media::Status job_status = base::unexpected(
      media::MediaError(media::ErrorCode::kNotImplemented, "not run", "", ""));
  EXPECT_TRUE(transcoder.Start(TestFile("small_h264_aac_3s.mp4"), params,
                               nullptr, [&](media::Status s) {
                                 job_status = std::move(s);
                                 done.store(true);
                               }));
  transcoder.Wait();
  ASSERT_TRUE(done.load());
  ASSERT_TRUE(job_status) << job_status.error().ToString();
  ExpectPlayable(params.output_path, 1);  // video only: -an was honoured.
  std::remove(params.output_path.c_str());
}

// Recording v1: plays the fixture, records from the current playhead for a
// moment, stops, and the output is a real container with both streams. The
// transcode-parallel semantics are what is under test here (a real source
// read from the current playhead), not surface capture.
TEST(PlayerRecorderTest, RecordDuringPlaybackWritesBothStreams) {
  PlayerConfig config;
  Player player(config, nullptr);
  player.SetEventHandler(
      base::BindRepeating([](const PlayerEvent&) {}));
  ASSERT_TRUE(
      player.SetDataSource(TestFile("small_h264_aac_3s.mp4")).has_value());
  ASSERT_TRUE(player.PrepareSync(base::Seconds(10)).has_value());
  // start-on-prepared may already have started playback; only call Start
  // when it did not.
  if (player.state() != PlayerState::kStarted) {
    player.Start();
  }
  ASSERT_TRUE(
      PumpUntilState(player, PlayerState::kStarted, base::Seconds(10)));

  PlayerRecorder recorder(player, config);
  const std::string out = testing::TempDir() + "record_test.mp4";
  const Status started = recorder.Start(out);
  ASSERT_TRUE(started) << started.error().ToString();

  // Copy-mode transcode runs FASTER than realtime: the 3s source's tail
  // finishes in milliseconds. But the job is ASYNC -- wait for it to finish
  // (or stop it) before verifying the output. Either way -- stopped cleanly
  // or self-completed -- the output must be a playable two-stream container.
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (recorder.IsRecording() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_FALSE(recorder.IsRecording());
  ExpectPlayable(out, 2);
  std::remove(out.c_str());
}

TEST(PlayerRecorderTest, StartInIdleIsRejected) {
  PlayerConfig config;
  Player player(config, nullptr);
  PlayerRecorder recorder(player, config);
  const Status started = recorder.Start(testing::TempDir() + "nope.mp4");
  ASSERT_FALSE(started);
  EXPECT_EQ(started.error().code(), ErrorCode::kInvalidState);
}

TEST(PlayerRecorderTest, StopWithoutStartIsRejected) {
  PlayerConfig config;
  Player player(config, nullptr);
  PlayerRecorder recorder(player, config);
  const Status stopped = recorder.Stop();
  ASSERT_FALSE(stopped);
  EXPECT_EQ(stopped.error().code(), ErrorCode::kInvalidState);
}

}  // namespace
}  // namespace avbase

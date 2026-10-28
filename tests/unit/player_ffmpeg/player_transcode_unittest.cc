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

}  // namespace
}  // namespace avbase

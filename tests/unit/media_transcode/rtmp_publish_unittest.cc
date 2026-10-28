// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/transcode/ffmpeg_transcode_job.h"

#include <gtest/gtest.h>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "media/ffmpeg/ffmpeg_demuxer.h"

namespace avbase::media {
namespace {

std::string TestFile(const char* name) {
  return std::string(AVBASE_TESTDATA_DIR) + "/" + name;
}

// RTMP publish at file level: the flv container over an rtmp:// URL is the
// same muxer path as a local file once the rtmp protocol is in the build.
// This test exercises the flv branch with a local file (deterministic, no
// network); the real-URL publish is verified against MediaMTX by
// tools/check_live_protocols-style manual runs (see docs/TODO §1).
TEST(RtmpPublishTest, FlvOutputRoundTripsThroughTheDemuxer) {
  TranscodeParams params;
  params.output_path = testing::TempDir() + "rtmp_sim.flv";
  params.output_format = "flv";
  params.video_codec.codec = "copy";
  params.audio_codec.codec = "copy";
  const Status status = Transcode(TestFile("small_h264_aac_3s.mp4"), params);
  ASSERT_TRUE(status) << status.error().ToString();

  base::scoped_refptr<MediaLog> media_log = base::MakeRefCounted<MediaLog>();
  FFmpegDemuxer demuxer(media_log);
  class Host final : public Demuxer::Host {
   public:
    void SetDuration(base::TimeDelta) override {}
    void OnBufferedTimeUpdate(base::TimeDelta, base::TimeDelta) override {}
    void OnDemuxerError(MediaError) override {}
  } host;
  base::test::TaskEnvironment env;
  Status opened = base::unexpected(
      MediaError(ErrorCode::kNotImplemented, "not run", "", ""));
  bool done = false;
  demuxer.Initialize(DataSourceDescriptor::FromUri(params.output_path), {},
                     &host, env.GetMainThreadTaskRunnerRef(),
                     base::BindOnce(
                         [](Status* out, bool* flag, Status s) {
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
  EXPECT_EQ(demuxer.media_info().streams.size(), 2u);
  std::remove(params.output_path.c_str());
}

TEST(RtmpPublishTest, MissingFormatForNetworkUrlFailsActionably) {
  TranscodeParams params;
  // rtmp:// with no output_format: the muxer cannot be inferred — this must
  // fail loudly rather than silently producing nothing.
  params.output_path = "rtmp://127.0.0.1/live/no_format";
  params.video_codec.codec = "copy";
  params.audio_codec.codec = "copy";
  const Status status = Transcode(TestFile("small_h264_aac_3s.mp4"), params);
  ASSERT_FALSE(status);
}

}  // namespace
}  // namespace avbase::media

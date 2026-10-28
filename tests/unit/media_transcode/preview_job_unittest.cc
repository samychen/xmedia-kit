// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/transcode/ffmpeg_preview_job.h"

#include <gtest/gtest.h>

#include "media/base/video_frame.h"

namespace avbase::media {
namespace {

std::string TestFile(const char* name) {
  return std::string(AVBASE_TESTDATA_DIR) + "/" + name;
}

TEST(PreviewJobTest, GrabsFramesAfterTheSeekPoint) {
  std::vector<base::scoped_refptr<VideoFrame>> frames;
  const Status status =
      PreviewFrames(TestFile("small_h264_aac_3s.mp4"), 1.0, 5, &frames);
  ASSERT_TRUE(status) << status.error().ToString();
  EXPECT_EQ(frames.size(), 5u);
  // The seek lands near 1s (keyframe seek: the first frame can be slightly
  // before), and every frame is a real decodable picture with sane geometry.
  for (const auto& f : frames) {
    EXPECT_EQ(f->coded_size(), (Size{320, 240}));
    EXPECT_GT(f->timestamp(), base::Milliseconds(700));
  }
  // Monotonic presentation order.
  for (size_t i = 1; i < frames.size(); ++i) {
    EXPECT_GT(frames[i]->timestamp(), frames[i - 1]->timestamp());
  }
}

TEST(PreviewJobTest, NoVideoStreamIsAnActionableError) {
  std::vector<base::scoped_refptr<VideoFrame>> frames;
  const Status status =
      PreviewFrames(TestFile("audio_only.m4a"), 0.0, 3, &frames);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::kSourceUnsupported);
}

TEST(PreviewJobTest, CancelStopsTheGrab) {
  TranscodeCancelToken token;
  token.Cancel();
  std::vector<base::scoped_refptr<VideoFrame>> frames;
  const Status status =
      PreviewFrames(TestFile("small_h264_aac_3s.mp4"), 0.0, 5, &frames, &token);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::kCancelled);
}

TEST(PreviewJobTest, MissingSourceIsAnOpenError) {
  std::vector<base::scoped_refptr<VideoFrame>> frames;
  const Status status =
      PreviewFrames("/nonexistent_for_tests/preview.mp4", 0.0, 3, &frames);
  ASSERT_FALSE(status);
  EXPECT_EQ(status.error().code(), ErrorCode::kSourceOpenFailed);
}

// A keyframe seek lands at or BEFORE the target. For a snapshot that is fine
// (it is what ffmpeg's -ss does), but a scrubber that asks for 0.5 s and gets
// 0 s shows the same picture for the whole first GOP. The first frame must be
// at or after the requested time, not merely "somewhere near" it.
TEST(PreviewJobTest, FirstFrameIsNotBeforeTheRequestedTime) {
  std::vector<base::scoped_refptr<VideoFrame>> frames;
  // 3-second source, ask for the middle: a GOP-sized error would be obvious.
  const Status status =
      PreviewFrames(TestFile("small_h264_aac_3s.mp4"), 1.5, 3, &frames);
  ASSERT_TRUE(status) << status.error().ToString();
  ASSERT_FALSE(frames.empty());
  for (size_t i = 0; i < frames.size(); ++i) {
    EXPECT_GE(frames[i]->timestamp(), base::Milliseconds(1500))
        << "frame " << i << " predates the requested seek point ("
        << frames[i]->timestamp().InMilliseconds() << " ms)";
  }
  // And it must not overshoot by more than a frame interval or so: a
  // preview labelled 1.5 s that actually shows 2.9 s is a different bug.
  EXPECT_LT(frames.front()->timestamp(), base::Milliseconds(1600));
}

// Ten-bit sources are covered deterministically by
// AvFrameToVideoFrameTest in ffmpeg_video_encoder_unittest.cc (synthetic
// frames, no committed binary): this function's converter is the one that
// test pins, so a preview of a 10-bit file gets the same fixed conversion.
// A file-based variant would need a 10-bit sample committed to testdata, and
// tests/testdata is a generated seed set, not a media library.

}  // namespace
}  // namespace avbase::media

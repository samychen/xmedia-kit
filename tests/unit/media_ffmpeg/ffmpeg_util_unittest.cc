// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/ffmpeg_util.h"

#include <gtest/gtest.h>

#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"

namespace xmedia::media::ffmpeg {
namespace {

// The bridges exist because each of these tables used to be written out once
// per consumer. These tests pin the table itself, so a future edit that "just
// adds one format" to one direction cannot silently desynchronize the other.

TEST(FfmpegUtilTest, PixelFormatRoundTrip) {
  const struct {
    media::VideoFormat ours;
    AVPixelFormat theirs;
  } kCases[] = {
      {media::VideoFormat::kI420, AV_PIX_FMT_YUV420P},
      {media::VideoFormat::kNV12, AV_PIX_FMT_NV12},
      {media::VideoFormat::kNV21, AV_PIX_FMT_NV21},
      {media::VideoFormat::kARGB, AV_PIX_FMT_0RGB32},
      {media::VideoFormat::kRGB24, AV_PIX_FMT_RGB24},
      {media::VideoFormat::kRGB565, AV_PIX_FMT_RGB565LE},
      {media::VideoFormat::kP010, AV_PIX_FMT_P010LE},
  };
  for (const auto& c : kCases) {
    EXPECT_EQ(ToAvPixelFormat(c.ours), c.theirs);
    EXPECT_EQ(FromAvPixelFormat(c.theirs, media::VideoFormat::kUnknown),
              c.ours);
  }
}

TEST(FfmpegUtilTest, PixelFormatYV12SharesYUV420P) {
  // Not a round trip: xmedia-kit's YV12 is YUV420P with V and U swapped, which the
  // table deliberately collapses. Anything that reads planes positionally must
  // not rely on this entry surviving a rename.
  EXPECT_EQ(ToAvPixelFormat(media::VideoFormat::kYV12), AV_PIX_FMT_YUV420P);
}

TEST(FfmpegUtilTest, PixelFormatUnexpressibleIsNoneNotAGuess) {
  EXPECT_EQ(ToAvPixelFormat(media::VideoFormat::kUnknown), AV_PIX_FMT_NONE);
  EXPECT_EQ(ToAvPixelFormat(media::VideoFormat::kYUY2), AV_PIX_FMT_NONE);
  EXPECT_EQ(ToAvPixelFormat(media::VideoFormat::kYUV420P10), AV_PIX_FMT_NONE);
}

TEST(FfmpegUtilTest, PixelFormatUnmappedFallsBack) {
  // 422/444 planar are "needs conversion", not "unknown": the render path
  // accepts kI420 and lets VideoConverter decide how to get there.
  EXPECT_EQ(FromAvPixelFormat(AV_PIX_FMT_YUV422P, media::VideoFormat::kUnknown),
            media::VideoFormat::kI420);
  EXPECT_EQ(FromAvPixelFormat(AV_PIX_FMT_YUV444P, media::VideoFormat::kUnknown),
            media::VideoFormat::kI420);
  // NONE and anything the table does not name return the caller's fallback,
  // which is how one table serves both "unknown is an error" (probe) and
  // "keep what we had" (decode).
  EXPECT_EQ(FromAvPixelFormat(AV_PIX_FMT_NONE, media::VideoFormat::kI420),
            media::VideoFormat::kI420);
  EXPECT_EQ(FromAvPixelFormat(AV_PIX_FMT_GRAY8, media::VideoFormat::kNV12),
            media::VideoFormat::kNV12);
}

TEST(FfmpegUtilTest, SampleFormatRoundTrip) {
  const struct {
    media::SampleFormat ours;
    AVSampleFormat theirs;
  } kCases[] = {
      {media::SampleFormat::kU8, AV_SAMPLE_FMT_U8},
      {media::SampleFormat::kS16, AV_SAMPLE_FMT_S16},
      {media::SampleFormat::kS32, AV_SAMPLE_FMT_S32},
      {media::SampleFormat::kF32, AV_SAMPLE_FMT_FLT},
      {media::SampleFormat::kS16P, AV_SAMPLE_FMT_S16P},
      {media::SampleFormat::kS32P, AV_SAMPLE_FMT_S32P},
      {media::SampleFormat::kF32P, AV_SAMPLE_FMT_FLTP},
  };
  for (const auto& c : kCases) {
    EXPECT_EQ(ToAvSampleFormat(c.ours), c.theirs);
    EXPECT_EQ(FromAvSampleFormat(c.theirs, media::SampleFormat::kUnknown),
              c.ours);
  }
}

TEST(FfmpegUtilTest, SampleFormatUnknownIsNoneNotAGuess) {
  // A container that declares no sample format (AV_SAMPLE_FMT_NONE in
  // AVCodecParameters::format) must reach the config as kUnknown, never as a
  // real format: the demuxer's job is to report what the file said.
  EXPECT_EQ(ToAvSampleFormat(media::SampleFormat::kUnknown),
            AV_SAMPLE_FMT_NONE);
  EXPECT_EQ(FromAvSampleFormat(AV_SAMPLE_FMT_NONE, media::SampleFormat::kS16),
            media::SampleFormat::kS16);
}

TEST(FfmpegUtilTest, AsFormatContextRoundTripsTheHandle) {
  AVFormatContext* raw = avformat_alloc_context();
  ASSERT_NE(raw, nullptr);
  EXPECT_EQ(AsFormatContext(raw), raw);
  avformat_free_context(raw);
}

}  // namespace
}  // namespace xmedia::media::ffmpeg

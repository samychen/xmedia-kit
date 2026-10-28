// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// End-to-end tests for the HLS playlist demuxer against the real testdata
// playlist (tests/testdata/hls: a two-segment MPEG-TS VOD). The suite
// proves the property the whole demuxer exists for: the pipeline sees ONE
// continuous timeline while the demuxer swaps inner FFmpegDemuxers at
// segment boundaries behind stable wrapper streams.

#include "media/ffmpeg/hls_playlist_demuxer.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "gtest/gtest.h"

namespace avbase::media {
namespace {

std::string TestFile(const char* name) {
  return std::string(AVBASE_TESTDATA_DIR) + "/" + name;
}

class RecordingHost final : public Demuxer::Host {
 public:
  void SetDuration(base::TimeDelta) override {}
  void OnBufferedTimeUpdate(base::TimeDelta, base::TimeDelta) override {}
  void OnDemuxerError(MediaError) override {}
};

class HlsPlaylistDemuxerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    media_log_ = base::MakeRefCounted<MediaLog>();
    demuxer_ =
        std::make_unique<HlsPlaylistDemuxer>(media_log_, BufferQueueLimits());
  }

  Status Open(const std::string& file_path) {
    Status result = Err(ErrorCode::kNotImplemented, "not run", {}, {});
    bool done = false;
    demuxer_->Initialize(DataSourceDescriptor::FromUri("file://" + file_path),
                         {}, &host_,
                         task_environment_.GetMainThreadTaskRunnerRef(),
                         base::BindOnce(
                             [](Status* out, bool* flag, Status s) {
                               *out = std::move(s);
                               *flag = true;
                             },
                             &result, &done));
    EXPECT_TRUE(
        PumpUntil([&done] { return done; }, 4000, std::chrono::milliseconds(5)))
        << "Initialize() never reported back";
    return result;
  }

  // One wrapped read, pumped to completion.
  struct ReadResult_ {
    DemuxerStream::Status status = DemuxerStream::Status::kError;
    DemuxerStream::DecoderBufferVector buffers;
  };
  ReadResult_ ReadOne(DemuxerStream* stream) {
    ReadResult_ out;
    bool done = false;
    stream->Read(
        1, base::BindOnce(
               [](ReadResult_* out, bool* flag, DemuxerStream::Status status,
                  DemuxerStream::DecoderBufferVector buffers) {
                 out->status = status;
                 out->buffers = std::move(buffers);
                 *flag = true;
               },
               &out, &done));
    PumpUntil([&done] { return done; }, 4000, std::chrono::milliseconds(5));
    return out;
  }

  template <typename Predicate>
  bool PumpUntil(Predicate predicate, int max_attempts,
                 std::chrono::milliseconds step) {
    for (int i = 0; i < max_attempts; ++i) {
      if (predicate()) {
        return true;
      }
      task_environment_.RunUntilIdle();
      std::this_thread::sleep_for(step);
    }
    return predicate();
  }

  base::test::TaskEnvironment task_environment_;
  base::scoped_refptr<MediaLog> media_log_;
  RecordingHost host_;
  std::unique_ptr<HlsPlaylistDemuxer> demuxer_;
};

TEST_F(HlsPlaylistDemuxerTest, OpensAndExposesTheFirstSegmentStreams) {
  ASSERT_TRUE(Open(TestFile("hls/local.m3u8")).has_value());
  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  ASSERT_NE(video, nullptr);
  EXPECT_TRUE(video->video_decoder_config().IsValidConfig());
  EXPECT_FALSE(demuxer_->IsLive());
  EXPECT_TRUE(demuxer_->IsSeekable());
  EXPECT_DOUBLE_EQ(demuxer_->media_info().duration.InSecondsF(), 2.0);
}

TEST_F(HlsPlaylistDemuxerTest, ReadsAcrossTheSegmentBoundaryOnOneTimeline) {
  ASSERT_TRUE(Open(TestFile("hls/local.m3u8")).has_value());
  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  ASSERT_NE(video, nullptr);

  base::TimeDelta previous;
  bool saw_packet_after_first_second = false;
  int reads = 0;
  while (reads++ < 500) {
    ReadResult_ result = ReadOne(video);
    ASSERT_EQ(result.status, DemuxerStream::Status::kOk);
    ASSERT_FALSE(result.buffers.empty());
    if (result.buffers.back()->IsEndOfStream()) {
      break;
    }
    for (const auto& buffer : result.buffers) {
      ASSERT_TRUE(buffer->has_valid_timestamp());
      // The wrapper rewrites timestamps into the playlist timeline, so the
      // two inner demuxers cannot disagree about where "now" is.
      EXPECT_GE(buffer->timestamp(), previous)
          << "timeline went backwards at read " << reads;
      previous = buffer->timestamp();
      if (previous >= base::Seconds(1)) {
        saw_packet_after_first_second = true;
      }
    }
  }
  EXPECT_TRUE(saw_packet_after_first_second)
      << "never crossed into the second segment";
  EXPECT_GE(previous, base::Seconds(1));
}

TEST_F(HlsPlaylistDemuxerTest, SeekLandsInTheRequestedSegment) {
  ASSERT_TRUE(Open(TestFile("hls/local.m3u8")).has_value());
  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  ASSERT_NE(video, nullptr);

  base::TimeDelta landed = base::TimeDelta::Min();
  bool done = false;
  demuxer_->StartPlayingFrom(base::Milliseconds(1500),
                             base::BindOnce(
                                 [](base::TimeDelta* out, bool* flag,
                                    Status status, base::TimeDelta actual) {
                                   if (status.has_value()) {
                                     *out = actual;
                                   }
                                   *flag = true;
                                 },
                                 &landed, &done));
  ASSERT_TRUE(
      PumpUntil([&done] { return done; }, 4000, std::chrono::milliseconds(5)));
  // The second segment starts at 1.0 s; the landing must be inside it.
  EXPECT_GE(landed, base::Seconds(1));
  EXPECT_LE(landed, base::Seconds(2));
  // And the read stream continues from there, on the same timeline.
  ReadResult_ result = ReadOne(video);
  ASSERT_EQ(result.status, DemuxerStream::Status::kOk);
  ASSERT_FALSE(result.buffers.empty());
  if (!result.buffers.back()->IsEndOfStream() &&
      result.buffers.front()->has_valid_timestamp()) {
    EXPECT_GE(result.buffers.front()->timestamp(), base::Seconds(1));
  }
}

TEST_F(HlsPlaylistDemuxerTest, AbrClimbsTheLadderAtABoundary) {
  // The master ladder is 30k/60k; the segments are ~15 KB per 1 s, so the
  // estimator lands around 120 kbps -- enough for the 2x headroom over the
  // top rung. Starting rung (initial bandwidth 10k) is the floor; the first
  // boundary must carry the session to the top.
  demuxer_ =
      std::make_unique<HlsPlaylistDemuxer>(media_log_, BufferQueueLimits());
  DemuxerOptions options;
  options.abr_enabled = true;
  options.abr_initial_bandwidth_bps = 10000;
  Status result = Err(ErrorCode::kNotImplemented, "not run", {}, {});
  bool done = false;
  demuxer_->Initialize(
      DataSourceDescriptor::FromUri("file://" + TestFile("hls/master.m3u8")),
      options, &host_, task_environment_.GetMainThreadTaskRunnerRef(),
      base::BindOnce(
          [](Status* out, bool* flag, Status s) {
            *out = std::move(s);
            *flag = true;
          },
          &result, &done));
  ASSERT_TRUE(
      PumpUntil([&done] { return done; }, 4000, std::chrono::milliseconds(5)));
  ASSERT_TRUE(result.has_value()) << result.error().summary();

  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  ASSERT_NE(video, nullptr);
  base::TimeDelta previous;
  int reads = 0;
  while (reads++ < 500) {
    ReadResult_ one = ReadOne(video);
    ASSERT_EQ(one.status, DemuxerStream::Status::kOk);
    ASSERT_FALSE(one.buffers.empty());
    if (one.buffers.back()->IsEndOfStream()) {
      break;
    }
    for (const auto& buffer : one.buffers) {
      if (buffer->has_valid_timestamp()) {
        EXPECT_GE(buffer->timestamp(), previous)
            << "timeline went backwards across the variant switch";
        previous = buffer->timestamp();
      }
    }
  }
  // The climb is the whole point: at least one rung switch at a boundary,
  // on a timeline that never went backwards.
  EXPECT_GE(demuxer_->GetStats().variant_switches, 1u);
  EXPECT_GE(previous, base::Seconds(1));
}

TEST_F(HlsPlaylistDemuxerTest, RejectsAPlaylistUrlThatIsNotM3U8) {
  Status status = Open(TestFile("hls/seg0.ts"));
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::kSourceUnsupported);
}

}  // namespace
}  // namespace avbase::media

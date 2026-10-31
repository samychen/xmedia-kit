// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// End-to-end tests for the DASH segment demuxer against the real testdata
// manifest (tests/testdata/dash: a two-segment MPEG-TS static MPD). The
// suite proves the property the demuxer exists for: the pipeline sees ONE
// continuous timeline while the demuxer swaps inner FFmpegDemuxers at
// SegmentTemplate boundaries behind stable wrapper streams.

#include "media/ffmpeg/dash_segment_demuxer.h"

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "base/functional/bind.h"
#include "base/test/task_environment.h"
#include "gtest/gtest.h"

namespace xmedia::media {
namespace {

std::string TestFile(const char* name) {
  return std::string(XMEDIA_TESTDATA_DIR) + "/" + name;
}

class RecordingHost final : public Demuxer::Host {
 public:
  void SetDuration(base::TimeDelta) override {}
  void OnBufferedTimeUpdate(base::TimeDelta, base::TimeDelta) override {}
  void OnDemuxerError(MediaError) override {}
};

class DashSegmentDemuxerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    media_log_ = base::MakeRefCounted<MediaLog>();
    demuxer_ =
        std::make_unique<DashSegmentDemuxer>(media_log_, BufferQueueLimits());
  }

  Status Open(const std::string& file_path) {
    Status result = Err(ErrorCode::kNotImplemented, "not run", {}, {});
    // Failure detail lands in the gtest message: a dash open has many
    // structured failure shapes and the bare has_value() tells none.
    result_error_sink_ = &result;
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

  Status Seek(base::TimeDelta time) {
    Status result = Err(ErrorCode::kNotImplemented, "not run", {}, {});
    bool done = false;
    demuxer_->StartPlayingFrom(
        time,
        base::BindOnce(
            [](Status* out, bool* flag, Status s, base::TimeDelta) {
              *out = std::move(s);
              *flag = true;
            },
            &result, &done));
    PumpUntil([&done] { return done; }, 4000, std::chrono::milliseconds(5));
    return result;
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

  // Points at Open()'s |result| while an open is in flight, so ASSERT_TRUE
  // prints the structured error instead of a bare boolean.
  Status* result_error_sink_ = nullptr;
  base::test::TaskEnvironment task_environment_;
  base::scoped_refptr<MediaLog> media_log_;
  RecordingHost host_;
  std::unique_ptr<DashSegmentDemuxer> demuxer_;
};

TEST_F(DashSegmentDemuxerTest, OpensAndExposesStreams) {
  Status open_result = Open(TestFile("dash/local.mpd"));
  ASSERT_TRUE(open_result.has_value()) << open_result.error().ToString();
  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  ASSERT_NE(video, nullptr);
  EXPECT_TRUE(video->video_decoder_config().IsValidConfig());
  // The muxed TS representation covers audio from the same chain.
  EXPECT_NE(demuxer_->GetStream(DemuxerStreamType::kAudio), nullptr);
  EXPECT_FALSE(demuxer_->IsLive());
  EXPECT_TRUE(demuxer_->IsSeekable());
  EXPECT_DOUBLE_EQ(demuxer_->media_info().duration.InSecondsF(), 2.0);
  EXPECT_STREQ(demuxer_->name(), "DashSegmentDemuxer");
}

TEST_F(DashSegmentDemuxerTest, ReadsAcrossTheSegmentBoundaryOnOneTimeline) {
  Status open_result = Open(TestFile("dash/local.mpd"));
  ASSERT_TRUE(open_result.has_value()) << open_result.error().ToString();
  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  ASSERT_NE(video, nullptr);

  // Read until EOS: timestamps must be monotonically non-decreasing across
  // the segment-1 boundary (the rebase), and no buffer may carry a
  // pre-rebase timestamp from the second segment's own zero.
  base::TimeDelta previous;
  bool saw_eos = false;
  int reads = 0;
  int64_t after_boundary = 0;
  while (reads++ < 500) {
    auto read = ReadOne(video);
    if (read.status != DemuxerStream::Status::kOk) {
      ADD_FAILURE() << "read failed with status "
                    << static_cast<int>(read.status);
      return;
    }
    for (const auto& buffer : read.buffers) {
      if (buffer->IsEndOfStream()) {
        saw_eos = true;
        continue;
      }
      if (buffer->has_valid_timestamp()) {
        EXPECT_GE(buffer->timestamp(), previous)
            << "timeline went backwards at read " << reads;
        if (buffer->timestamp() >= base::Seconds(1)) {
          ++after_boundary;
        }
        previous = buffer->timestamp();
      }
    }
    if (saw_eos) {
      break;
    }
  }
  EXPECT_TRUE(saw_eos);
  EXPECT_GT(after_boundary, 0) << "the second segment contributed nothing";
}

TEST_F(DashSegmentDemuxerTest, SeekLandsInTheTargetSegment) {
  Status open_result = Open(TestFile("dash/local.mpd"));
  ASSERT_TRUE(open_result.has_value()) << open_result.error().ToString();
  Status seek_result = Seek(base::Milliseconds(1500));
  ASSERT_TRUE(seek_result.has_value()) << seek_result.error().ToString();
  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  ASSERT_NE(video, nullptr);
  auto read = ReadOne(video);
  ASSERT_EQ(read.status, DemuxerStream::Status::kOk);
  ASSERT_FALSE(read.buffers.empty());
  const auto* buffer = read.buffers.front().get();
  ASSERT_FALSE(buffer->IsEndOfStream());
  EXPECT_GE(buffer->timestamp(), base::Seconds(1))
      << "the seek landed before the target segment";
  EXPECT_LT(buffer->timestamp(), base::Seconds(2));
}

TEST_F(DashSegmentDemuxerTest, RejectsDynamicMpdWithAStructuredError) {
  // A dynamic MPD cannot be served from a fixture file whose segment list
  // is fixed; the structured refusal is the registered live-DASH follow-up
  // boundary, asserted here so it cannot silently degrade to the FFmpeg
  // fallback or a raw parse error.
  const std::string path = testing::TempDir() + "/dynamic.mpd";
  FILE* f = fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  const char* body =
      "<MPD xmlns=\"urn:mpeg:dash:schema:mpd:2011\" type=\"dynamic\">"
      "<Period><AdaptationSet mimeType=\"video/mp2t\">"
      "<Representation id=\"v\" bandwidth=\"1\">"
      "<SegmentTemplate media=\"seg$Number$.ts\" duration=\"1\" "
      "timescale=\"1\"/></Representation>"
      "</AdaptationSet></Period></MPD>";
  fwrite(body, 1, strlen(body), f);
  fclose(f);
  Status result = Open(path);
  std::remove(path.c_str());
  ASSERT_FALSE(result.has_value()) << result.error().ToString();
  EXPECT_EQ(result.error().code(), ErrorCode::kSourceUnsupported);
}

}  // namespace
}  // namespace xmedia::media

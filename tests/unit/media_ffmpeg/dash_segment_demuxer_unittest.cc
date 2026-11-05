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
#include "tests/support/local_http_server.h"

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
        time, base::BindOnce(
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
  // REAL time, not the default kMockTime: the live reload is scheduled with
  // PostDelayedTask, and a mock clock only advances when a test says so --
  // RunUntilIdle() would then run forever without ever reaching the reload, and
  // the extension assertion would fail for a reason that has nothing to do
  // with the demuxer.
  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::kRealTime};
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
  // A dynamic MPD that names no timing anchor cannot be placed on a grid, so
  // it is refused with a structured error rather than silently degrading to
  // the FFmpeg fallback. (Since live DASH landed, the refusal is about the
  // MISSING ANCHOR, not about "dynamic" as such -- the playable case is
  // PlaysADynamicMpdWindowAndExtendsItOnReload below.)
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

// The live half: a dynamic manifest whose window GROWS. Before live DASH
// landed every dynamic MPD was refused; this pins the two properties that make
// the reload real -- the window is located from the anchors, and a refresh
// EXTENDS the chain instead of rebuilding it (a rebuild would renumber the
// cursor and make the timeline jump).
TEST_F(DashSegmentDemuxerTest, PlaysADynamicMpdWindowAndExtendsItOnReload) {
  test::LocalHttpServer server;
  if (!server.Start()) {
    GTEST_SKIP() << "loopback sockets unavailable";
  }
  // Real TS segments so the inner FFmpegDemuxer can open them.
  auto load = [](const char* name) {
    std::string body;
    FILE* f = fopen(TestFile(name).c_str(), "rb");
    if (f == nullptr) {
      return body;
    }
    char buf[8192];
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
      body.append(buf, n);
    }
    fclose(f);
    return body;
  };
  const std::string seg0 = load("dash/seg0.ts");
  const std::string seg1 = load("dash/seg1.ts");
  ASSERT_FALSE(seg0.empty());
  ASSERT_FALSE(seg1.empty());
  for (const char* path : {"/seg0.ts", "/seg1.ts"}) {
    test::LocalHttpServer::File file;
    file.body = path[1] == 's' && path[5] == '0' ? seg0 : seg1;
    file.content_type = "video/mp2t";
    server.SetFile(path, file);
  }

  // Two segments published at first, one more after the reload. Both manifests
  // keep the SAME availabilityStartTime, which is what makes them the same
  // presentation.
  auto manifest = [](int published) {
    const int tail = published - 1;
    return std::string(
               "<MPD type=\"dynamic\" availabilityStartTime="
               "\"2026-10-31T09:00:00Z\" publishTime=\"2026-10-31T09:00:0") +
           std::to_string(published) +
           "Z\" minimumUpdatePeriod=\"PT0.5S\">"
           "<Period><AdaptationSet mimeType=\"video/mp2t\">"
           "<SegmentTemplate media=\"seg$Number$.ts\" duration=\"1\" "
           "timescale=\"1\" startNumber=\"0\"/>" +
           "<Representation id=\"v0\" bandwidth=\"1000000\"/>" +
           "</AdaptationSet></Period></MPD>" + std::string("<!-- published ") +
           std::to_string(tail) + " -->";
  };
  test::LocalHttpServer::File mpd;
  mpd.body = manifest(2);
  server.SetFile("/live.mpd", mpd);

  Status result = Err(ErrorCode::kNotImplemented, "not run", {}, {});
  bool done = false;
  const std::string url = server.UrlFor("/live.mpd");
  demuxer_->Initialize(DataSourceDescriptor::FromUri(url), {}, &host_,
                       task_environment_.GetMainThreadTaskRunnerRef(),
                       base::BindOnce(
                           [](Status* out, bool* flag, Status s) {
                             *out = std::move(s);
                             *flag = true;
                           },
                           &result, &done));
  ASSERT_TRUE(
      PumpUntil([&done] { return done; }, 4000, std::chrono::milliseconds(5)))
      << "a dynamic MPD with anchors was never initialised";
  ASSERT_TRUE(result.has_value()) << result.error().ToString();
  EXPECT_TRUE(demuxer_->IsLive()) << "a dynamic profile must report live";

  DemuxerStream* video = demuxer_->GetStream(DemuxerStreamType::kVideo);
  ASSERT_NE(video, nullptr) << "the dynamic window exposed no video stream";

  // Read what the first window published. A live tail PARKS the read rather
  // than answering EOS, so this loop must count packets instead of waiting for
  // an end that must never come.
  const uint64_t after_init = demuxer_->GetStats().packets_demuxed;
  for (int i = 0; i < 8; ++i) {
    ReadResult_ r = ReadOne(video);
    if (r.status != DemuxerStream::Status::kOk || r.buffers.empty()) {
      break;
    }
    ASSERT_FALSE(r.buffers.front()->IsEndOfStream())
        << "a live window must not report EOS at its published tail";
  }
  EXPECT_GT(demuxer_->GetStats().packets_demuxed, after_init)
      << "the live window served no packets";

  // THE RELOAD HALF. Publish a longer manifest, let the minimumUpdatePeriod
  // tick (0.5 s, the clamp floor), and require that the NEW segments are
  // actually playable: a reload that only re-parsed would leave the chain at
  // its old tail and the read parked forever.
  const uint64_t before = demuxer_->GetStats().packets_demuxed;
  mpd.body = manifest(4);
  server.SetFile("/live.mpd", mpd);
  bool served_more = false;
  for (int i = 0; i < 60 && !served_more; ++i) {
    ReadResult_ r = ReadOne(video);
    if (r.status == DemuxerStream::Status::kOk && !r.buffers.empty() &&
        !r.buffers.front()->IsEndOfStream()) {
      served_more = true;
    }
  }
  EXPECT_TRUE(served_more) << "the manifest reload did not make the new "
                              "segments playable (packets_demuxed "
                           << before << " -> "
                           << demuxer_->GetStats().packets_demuxed << ")";
}

}  // namespace
}  // namespace xmedia::media

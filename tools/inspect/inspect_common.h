// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Shared plumbing for the xmedia-inspect subcommands.

#ifndef XMEDIA_TOOLS_INSPECT_INSPECT_COMMON_H_
#define XMEDIA_TOOLS_INSPECT_INSPECT_COMMON_H_

#include <stdio.h>

#include <stddef.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/task_environment.h"
#include "base/time/time.h"
#include "media/base/media_error.h"
#include "media/base/media_info.h"
#include "media/ffmpeg/ffmpeg_demuxer.h"
#include "media/transcode/ffmpeg_transcode_job.h"

namespace xmedia {

// Parsed command line. Shared by every subcommand.
struct Options {
  std::string subcommand;
  std::string path;
  bool video_only = false;
  bool audio_only = false;
  size_t limit = 100000;
};

void Usage();

// Parses argv. Unknown flags are rejected rather than ignored: a silently
// dropped --limit would make a run look like it finished when it did not.
// Returns false when the caller should exit without doing any work -- either a
// parse error, or -h having printed the usage text.
bool ParseArgs(int argc, char** argv, Options* out);

const char* StreamKindName(media::StreamKind kind);

// Host that prints demuxer errors instead of swallowing them. A silent Host
// would make "no frames decoded" look like an empty file.
class InspectHost final : public media::Demuxer::Host {
 public:
  void SetDuration(base::TimeDelta duration) override { duration_ = duration; }
  void OnBufferedTimeUpdate(base::TimeDelta, base::TimeDelta) override {}
  void OnDemuxerError(media::MediaError error) override {
    fprintf(stderr, "[demuxer error] %s\n", error.ToString().c_str());
    errors_.push_back(std::move(error));
  }
  const std::vector<media::MediaError>& errors() const { return errors_; }
  base::TimeDelta duration() const { return duration_; }

 private:
  std::vector<media::MediaError> errors_;
  base::TimeDelta duration_;
};

// Drives the demuxer's task queue. The demuxer works on a background sequence,
// so its callbacks only land when the queue is pumped.
class Pump {
 public:
  explicit Pump(base::test::TaskEnvironment& env) : env_(env) {}

  // Runs the queue and yields between spins. RunUntilIdle() alone returns
  // immediately without the callback ever having been posted, so spinning
  // without yielding burns the whole budget in microseconds and reports a
  // timeout on a file that opened perfectly well.
  template <typename Pred>
  bool Until(const Pred& pred, int max_spins = 20000) {
    for (int i = 0; i < max_spins && !pred(); ++i) {
      env_.RunUntilIdle();
      if (!pred()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    return pred();
  }

 private:
  base::test::TaskEnvironment& env_;
};

// Opens the demuxer, or prints why it could not. On success |info| points at
// the demuxer's own MediaInfo, valid for as long as the demuxer lives.
bool OpenDemuxer(media::FFmpegDemuxer* demuxer, const std::string& path,
                 InspectHost* host, Pump* pump,
                 base::scoped_refptr<base::SequencedTaskRunner> runner,
                 const media::MediaInfo** info);

int RunProbe(const Options& opts);
int RunDecode(const Options& opts);
int RunSync(const Options& opts);

// The numeric parsers the subcommands share. They were written three times
// (transcode, preview, record) and had already drifted: only one of the three
// checked ERANGE, and record rejected 0 while preview accepted it. Three
// copies of a parser is three chances for a CLI to accept "12s" in one
// subcommand and reject it in another.
//
// |allow_zero| is the one real difference: a seek target may be 0 (the start),
// a duration may not.
bool ParseSecondsArg(const char* text, bool allow_zero, double* out);
// A positive integer count, with |max_value| as the ceiling (0 = no ceiling).
bool ParseCountArg(const char* text, int max_value, int* out);

// Builds the transcode parameters the three job-driving subcommands share.
// They each wrote this by hand, and had already disagreed: `record` spells
// "drop this stream" as --vcodec none while `publish` spells it --no-video, so
// the same argv meant different things depending on which verb ran. One
// builder, one spelling ("copy" passes through, "none"/empty drops).
// |realtime| and |muxer_options| stay explicit: those genuinely differ per
// verb.
media::TranscodeParams BuildJobParams(const std::string& output,
                                      const std::string& format,
                                      const std::string& vcodec,
                                      const std::string& acodec,
                                      double duration, bool realtime);

// transcode / preview / publish take their own options, so they take argv
// themselves (argv[0] is the subcommand name) rather than the shared Options.
int RunTranscode(int argc, char** argv);
int RunPreview(int argc, char** argv);
int RunPublish(int argc, char** argv);
int RunRecord(int argc, char** argv);

}  // namespace xmedia

#endif  // XMEDIA_TOOLS_INSPECT_INSPECT_COMMON_H_

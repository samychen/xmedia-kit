// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/public/player_transcode.h"

#include <memory>
#include <utility>

#include "media/transcode/ffmpeg_transcode_job.h"

namespace avbase {

namespace {

// -vn / -an: the PlayerConfig disabled-stream flags, applied to the job's
// codec specs the same way transcode.cc's own flags empty them.
media::TranscodeParams ApplyDisabledStreams(const PlayerConfig& config,
                                            media::TranscodeParams params) {
  if (config.video.disabled) {
    params.video_codec.codec.clear();
  }
  if (config.audio.disabled) {
    params.audio_codec.codec.clear();
  }
  return params;
}

}  // namespace

PlayerStatus TranscodeMedia(const std::string& input_uri,
                            const media::TranscodeParams& params,
                            media::TranscodeProgressCB progress,
                            const media::TranscodeCancelToken* cancel) {
  return media::Transcode(input_uri, params, std::move(progress), cancel);
}

PlayerRecorder::PlayerRecorder(const Player& player, const PlayerConfig& config)
    : player_(player), config_(config),
      job_(std::make_unique<media::TranscodeJob>()) {}

PlayerRecorder::~PlayerRecorder() = default;

Status PlayerRecorder::Start(const std::string& output_path,
                             const media::TranscodeParams& overrides) {
  const PlayerState state = player_.state();
  if (state != PlayerState::kStarted && state != PlayerState::kPaused) {
    return base::unexpected(
        MediaError(ErrorCode::kInvalidState,
                   std::string("StartRecord called in state ") +
                       GetPlayerStateName(state),
                   "recording needs a playhead to start from",
                   "start recording while the player is playing or paused"));
  }
  const auto info = player_.media_info();
  if (!info || info->uri.empty()) {
    return base::unexpected(
        MediaError(ErrorCode::kInvalidArgument,
                   "the player's source carries no URI",
                   "recording v1 reads the source linearly from the current "
                   "playhead, which needs an addressable input",
                   "memory/descriptor sources need the render-tap recorder "
                   "(follow-up)"));
  }
  if (job_->IsRunning()) {
    return base::unexpected(MediaError(
        ErrorCode::kInvalidState, "a record is already running",
        "one recorder, one job",
        "call Stop() before starting the next record"));
  }
  media::TranscodeParams params =
      BuildParams(output_path, overrides);
  const double start =
      std::max(0.0, player_.GetMediaTime().InSecondsF());
  params.start_time_seconds = start;
  job_->Start(info->uri, params);
  return OkStatus();
}

Status PlayerRecorder::Stop() {
  if (!job_->IsRunning()) {
    return base::unexpected(MediaError(ErrorCode::kInvalidState,
                                       "no record is running", {},
                                       "start one first"));
  }
  job_->Cancel();
  job_->Wait();
  return OkStatus();
}

bool PlayerRecorder::IsRecording() const {
  return job_ && job_->IsRunning();
}

media::TranscodeParams PlayerRecorder::BuildParams(
    const std::string& output_path,
    const media::TranscodeParams& overrides) const {
  media::TranscodeParams params = overrides;
  params.output_path = output_path;
  // Stream copy is the v1 default: "record what is playing" for the common
  // mp4 -> mp4 case is lossless and cheap. The overrides count as "nothing
  // specified" while they are byte-equal to a default-constructed params
  // (whose spec defaults are libx264/aac); anything a caller set explicitly
  // — including codec="copy" — wins verbatim.
  const media::TranscodeParams untouched;
  const bool caller_customized =
      overrides.video_codec.codec != untouched.video_codec.codec ||
      overrides.video_codec.bit_rate != untouched.video_codec.bit_rate ||
      overrides.video_codec.crf != untouched.video_codec.crf ||
      overrides.video_codec.gop != untouched.video_codec.gop ||
      overrides.video_codec.width != 0 || overrides.video_codec.height != 0 ||
      !overrides.video_codec.preset.empty() ||
      overrides.audio_codec.codec != untouched.audio_codec.codec ||
      overrides.audio_codec.bit_rate != untouched.audio_codec.bit_rate ||
      overrides.audio_codec.sample_rate != 0 ||
      overrides.audio_codec.channels != 0;
  if (!caller_customized) {
    params.video_codec.codec = "copy";
    params.audio_codec.codec = "copy";
  }
  // Stop() cancels the job; the record must stay a playable file after it.
  params.finalize_on_cancel = true;
  if (config_.video.disabled) {
    params.video_codec.codec.clear();
  }
  if (config_.audio.disabled) {
    params.audio_codec.codec.clear();
  }
  return params;
}

PlayerTranscoder::PlayerTranscoder(const PlayerConfig& config)
    : config_(config), job_(std::make_unique<media::TranscodeJob>()) {}

PlayerTranscoder::~PlayerTranscoder() = default;

bool PlayerTranscoder::Start(const std::string& input_uri,
                             const media::TranscodeParams& params,
                             media::TranscodeProgressCB progress,
                             media::TranscodeDoneCB done_cb) {
  return job_->Start(input_uri, ApplyDisabledStreams(config_, params),
                     std::move(progress), std::move(done_cb));
}

void PlayerTranscoder::Cancel() {
  job_->Cancel();
}

void PlayerTranscoder::Wait() {
  job_->Wait();
}

bool PlayerTranscoder::IsRunning() const {
  return job_->IsRunning();
}

}  // namespace avbase

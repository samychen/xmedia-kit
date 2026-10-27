// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/public/player_transcode.h"

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

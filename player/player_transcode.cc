// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/public/player_transcode.h"

#include <cstdlib>
#include <memory>
#include <utility>

#include "base/functional/bind.h"
#include "base/synchronization/waitable_event.h"
#include "base/threading/thread.h"
#include "media/transcode/ffmpeg_transcode_job.h"

namespace avbase {

namespace {
constexpr int64_t kRateFallback = 48000;


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

PlayerRecorder::PlayerRecorder(Player& player, const PlayerConfig& config)
    : player_(player), config_(config) {
  tap_.on_stream = [this](const media::RecordStreamHeader& header) {
    if (!record_runner_) {
      return;
    }
    record_runner_->PostTask(
        FROM_HERE, base::BindOnce(&PlayerRecorder::OnTapStream,
                                  base::Unretained(this), header));
  };
  tap_.on_packet = [this](bool video,
                          base::scoped_refptr<media::DecoderBuffer> buffer) {
    if (!record_runner_) {
      return;
    }
    record_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&PlayerRecorder::OnTapPacket, base::Unretained(this),
                       video, std::move(buffer)));
  };
}

PlayerRecorder::~PlayerRecorder() {
  if (recording_.load()) {
    Stop();
  }
}

Status PlayerRecorder::Start(const std::string& output_path) {
  const PlayerState state = player_.state();
  if (state != PlayerState::kStarted && state != PlayerState::kPaused) {
    return base::unexpected(
        MediaError(ErrorCode::kInvalidState,
                   std::string("StartRecord called in state ") +
                       GetPlayerStateName(state),
                   "recording follows a playing pipeline",
                   "start recording while the player is playing or paused"));
  }
  if (recording_.load()) {
    return base::unexpected(MediaError(ErrorCode::kInvalidState,
                                       "a record is already running", {},
                                       "call Stop() first"));
  }
  auto thread = std::make_unique<base::Thread>("avbase-recorder");
  if (!thread->Start()) {
    return base::unexpected(
        MediaError(ErrorCode::kSourceReadFailed,
                        "record thread failed to start",
                   {}, {}));
  }
  record_thread_ = std::move(thread);
  record_runner_ = record_thread_->task_runner();
  muxer_ = std::make_unique<media::FFmpegEncodeMuxer>();
  if (!muxer_->Open(output_path)) {
    muxer_.reset();
    record_runner_.reset();
    record_thread_.reset();
    return base::unexpected(MediaError(
        ErrorCode::kInvalidArgument, "cannot open the record output",
        "avio for " + output_path + " failed",
        "check the directory exists, and the extension maps to an enabled "
        "muxer"));
  }
  finished_ = false;
  audio_ended_ = video_ended_ = false;
  audio_stream_ = video_stream_ = -1;
  audio_last_pts_ = video_last_pts_ = media::kNoPtsValue;
  audio_shift_ = video_shift_ = 0;
  recording_.store(true);
  player_.SetRecordTap(tap_);
  return OkStatus();
}

Status PlayerRecorder::Stop() {
  if (!recording_.load()) {
    return base::unexpected(MediaError(ErrorCode::kInvalidState,
                                       "no record is running", {},
                                       "start one first"));
  }
  player_.SetRecordTap(media::RecordTap{});
  // Finalize ON the record thread (FIFO: every queued packet is written
  // first), then stop the thread.
  base::WaitableEvent done;
  record_runner_->PostTask(
      FROM_HERE, base::BindOnce(
                     [](media::FFmpegEncodeMuxer* muxer, bool* finished,
                        base::WaitableEvent* ev) {
                       if (!*finished) {
                         muxer->Finish();
                         *finished = true;
                       }
                       ev->Signal();
                     },
                     base::Unretained(muxer_.get()), &finished_, &done));
  done.Wait();
  record_thread_->Stop();
  record_runner_.reset();
  record_thread_.reset();
  muxer_.reset();
  recording_.store(false);
  return OkStatus();
}

bool PlayerRecorder::IsRecording() const {
  return recording_.load();
}

void PlayerRecorder::OnTapStream(const media::RecordStreamHeader& header) {
  if (!muxer_ || finished_) {
    return;
  }
  if (header.video) {
    media::FFmpegEncodeMuxer::VideoStreamParams stream;
    stream.codec_name = header.codec_name;
    stream.width = header.width;
    stream.height = header.height;
    stream.extradata = header.extra_data;
    video_stream_ = muxer_->AddVideoStream(stream);
  } else {
    media::FFmpegEncodeMuxer::AudioStreamParams stream;
    stream.codec_name = header.codec_name;
    stream.sample_rate = header.sample_rate;
    stream.channels = header.channels;
    stream.extradata = header.extra_data;
    audio_stream_ = muxer_->AddAudioStream(stream);
    audio_rate_ = header.sample_rate;
  }
}

void PlayerRecorder::OnTapPacket(
    bool video, base::scoped_refptr<media::DecoderBuffer> buffer) {
  if (!muxer_ || finished_) {
    return;
  }
  const bool is_eos = !buffer || buffer->IsEndOfStream();
  if (is_eos) {
    if (video) {
      video_ended_ = true;
    } else {
      audio_ended_ = true;
    }
    // End of playback finalizes the file: IsRecording() flips to false and
    // the host's Stop() becomes a no-op-safe finalize.
    muxer_->Finish();
    finished_ = true;
    recording_.store(false);
    return;
  }
  const int stream = video ? video_stream_ : audio_stream_;
  if (stream < 0) {
    return;  // Packets before the decoder-open header; drop.
  }
  int64_t pts = 0;
  if (video) {
    pts = buffer->timestamp().InMicroseconds() * 90 / 1000;
    if (video_last_pts_ != media::kNoPtsValue && pts < video_last_pts_) {
      video_shift_ += video_last_pts_ - pts + 1;  // Segment re-base (seek).
      pts = video_last_pts_ + 1;
    }
    video_last_pts_ = pts;
  } else {
    const int64_t rate = audio_rate_ > 0 ? audio_rate_ : kRateFallback;
    pts = buffer->timestamp().InMicroseconds() * rate / 1000000;
    if (audio_last_pts_ != media::kNoPtsValue && pts < audio_last_pts_) {
      audio_shift_ += audio_last_pts_ - pts + 1;
      pts = audio_last_pts_ + 1;
    }
    audio_last_pts_ = pts;
  }
  media::EncodedPacket ep;
  ep.data.assign(buffer->data().begin(), buffer->data().end());
  ep.pts = pts + (video ? video_shift_ : audio_shift_);
  ep.dts = ep.pts;
  ep.flags = 1;  // Keyframe: a tap cannot know better, and players accept it.
  if (!muxer_->WritePacket(stream, ep)) {
    LOG(ERROR) << "recorder: packet write failed; finalizing";
    muxer_->Finish();
    finished_ = true;
    recording_.store(false);
  }
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

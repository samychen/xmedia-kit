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
  video_pending_.clear();
  audio_pending_.clear();
  gate_open_ = false;
  grace_open_ = false;
  backstop_scheduled_ = false;
  recording_.store(true);
  player_.SetRecordTap(tap_);
  // Re-anchor at the current playhead. The tap only sees packets the decoder
  // streams pull AFTER it attached -- audio primes up to 3 s ahead
  // (audio_renderer_algorithm kMaxCapacity), so a mid-playback attach to a
  // short file would otherwise find its whole audio already decoded (and its
  // EOS consumed), leaving the container with a dead track. The seek's flush
  // discards the primed decode and both legs re-pull through the tap, so the
  // record is exactly [attach .. end] on every leg. Hosts observe it as a
  // seek to the current position.
  const auto seek =
      player_.SeekTo(player_.GetMediaTime(), SeekMode::kPreviousKeyframe,
                     Player::SeekCB());
  if (!seek) {
    LOG(WARNING) << "recorder: re-anchor seek failed ("
                 << seek.error().ToString() << "); recording un-anchored";
  }
  // The legs' stream headers fire on their own decoder sequences (S3/S4) and
  // race the first packets; the gate below keeps the muxer header from being
  // written until the stream set is final. A leg that never delivers a header
  // (the source has no such track) must not hold the file hostage: after the
  // grace period the gate opens with whatever streams exist.
  record_runner_->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&PlayerRecorder::OnGraceExpired, base::Unretained(this)),
      base::Milliseconds(300));
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
                     [](PlayerRecorder* self, bool* finished,
                        base::WaitableEvent* ev) {
                       if (!*finished) {
                         // Force the gate open so queued packets still land
                         // in the container before it finalizes.
                         self->FinalizeRecord();
                         *finished = true;
                       }
                       ev->Signal();
                     },
                     base::Unretained(this), &finished_, &done));
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
  // One muxer stream per leg, registered before the header opens. A second
  // header on the same leg (track switch mid-record) keeps the current
  // stream: the container cannot grow after its header is written, and the
  // muxer refuses post-header additions outright.
  if (header.video) {
    if (video_stream_ >= 0 || gate_open_) {
      return;
    }
    media::FFmpegEncodeMuxer::VideoStreamParams stream;
    stream.codec_name = header.codec_name;
    stream.width = header.width;
    stream.height = header.height;
    stream.extradata = header.extra_data;
    video_stream_ = muxer_->AddVideoStream(stream);
    if (video_stream_ < 0) {
      LOG(ERROR) << "recorder: video stream registration failed";
    }
  } else {
    if (audio_stream_ >= 0 || gate_open_) {
      return;
    }
    media::FFmpegEncodeMuxer::AudioStreamParams stream;
    stream.codec_name = header.codec_name;
    stream.sample_rate = header.sample_rate;
    stream.channels = header.channels;
    stream.extradata = header.extra_data;
    audio_stream_ = muxer_->AddAudioStream(stream);
    if (audio_stream_ < 0) {
      LOG(ERROR) << "recorder: audio stream registration failed";
    } else {
      audio_rate_ = header.sample_rate;
    }
  }
  MaybeOpenGateAndFlush();
}

bool PlayerRecorder::ExpectedLegsComplete() const {
  const bool want_video = !config_.video.disabled;
  const bool want_audio = !config_.audio.disabled;
  return (!want_video || video_stream_ >= 0) &&
         (!want_audio || audio_stream_ >= 0);
}

void PlayerRecorder::MaybeOpenGateAndFlush() {
  if (finished_) {
    return;
  }
  if (gate_open_) {
    // Already open: flush anything still queued for registered legs.
    std::vector<base::scoped_refptr<media::DecoderBuffer>> pending;
    pending.swap(video_pending_);
    for (auto& b : pending) {
      WriteBuffered(true, std::move(b));
      if (finished_) {
        return;
      }
    }
    pending.swap(audio_pending_);
    for (auto& b : pending) {
      WriteBuffered(false, std::move(b));
      if (finished_) {
        return;
      }
    }
    return;
  }
  if (!ExpectedLegsComplete() && !grace_open_) {
    return;  // The stream set may still grow; keep holding packets.
  }
  gate_open_ = true;
  // Legs that never registered a stream lose their queued packets: the
  // container's stream set is final and the muxer refuses late additions.
  for (const bool video : {true, false}) {
    const int stream = video ? video_stream_ : audio_stream_;
    auto& pending = video ? video_pending_ : audio_pending_;
    if (stream < 0 && !pending.empty()) {
      LOG(WARNING) << "recorder: dropping " << pending.size() << " queued "
                   << (video ? "video" : "audio") << " packets (no stream)";
      pending.clear();
    }
  }
  std::vector<base::scoped_refptr<media::DecoderBuffer>> pending;
  pending.swap(video_pending_);
  for (auto& b : pending) {
    WriteBuffered(true, std::move(b));
    if (finished_) {
      return;
    }
  }
  pending.swap(audio_pending_);
  for (auto& b : pending) {
    WriteBuffered(false, std::move(b));
    if (finished_) {
      return;
    }
  }
}

void PlayerRecorder::OnGraceExpired() {
  if (!muxer_ || finished_) {
    return;
  }
  grace_open_ = true;
  MaybeOpenGateAndFlush();
}

void PlayerRecorder::FinalizeRecord() {
  if (finished_) {
    return;
  }
  grace_open_ = true;
  MaybeOpenGateAndFlush();
  muxer_->Finish();
  finished_ = true;
  recording_.store(false);
}

void PlayerRecorder::OnFinalizeBackstop() {
  if (!muxer_ || finished_) {
    return;
  }
  // One leg ended and the other delivered nothing more within the backstop
  // window (its EOS was likely consumed before the tap attached): close the
  // record with what exists rather than hanging until Stop.
  LOG(WARNING) << "recorder: finalize backstop; closing the record";
  FinalizeRecord();
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
    // First end: the other leg may still owe data (a burst drain delivers
    // one leg entirely before the other starts, and a primed leg's EOS can
    // be consumed before the tap attached) -- the backstop closes the
    // record if nothing more arrives. Both ends: finalize now.
    if (video_ended_ && audio_ended_) {
      FinalizeRecord();
      return;
    }
    if (!backstop_scheduled_) {
      backstop_scheduled_ = true;
      record_runner_->PostDelayedTask(
          FROM_HERE,
          base::BindOnce(&PlayerRecorder::OnFinalizeBackstop,
                         base::Unretained(this)),
          kFinalizeBackstop);
    }
    return;
  }
  const int stream = video ? video_stream_ : audio_stream_;
  if (stream < 0) {
    auto& pending = video ? video_pending_ : audio_pending_;
    if (pending.size() < kMaxPending) {
      pending.push_back(std::move(buffer));
    } else {
      LOG(WARNING) << "recorder: " << (video ? "video" : "audio")
                   << " pending queue full; dropping packets";
    }
    return;
  }
  if (!gate_open_) {
    if (ExpectedLegsComplete()) {
      MaybeOpenGateAndFlush();
    } else {
      // Another enabled leg's stream header is still in flight (S3 vs S4
      // race); hold the packet so the header carries the full stream set.
      auto& pending = video ? video_pending_ : audio_pending_;
      if (pending.size() < kMaxPending) {
        pending.push_back(std::move(buffer));
      }
      return;
    }
  }
  WriteBuffered(video, std::move(buffer));
}

void PlayerRecorder::WriteBuffered(
    bool video, base::scoped_refptr<media::DecoderBuffer> buffer) {
  const int stream = video ? video_stream_ : audio_stream_;
  if (stream < 0) {
    return;  // Flushed for a leg that never registered; nothing to write.
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
    FinalizeRecord();
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

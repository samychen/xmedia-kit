// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_PLAYER_PUBLIC_PLAYER_TRANSCODE_H_
#define AVBASE_PLAYER_PUBLIC_PLAYER_TRANSCODE_H_

#include <memory>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "media/base/media_error.h"
#include "media/transcode/ffmpeg_transcode_job.h"
#include "player/public/error.h"
#include <memory>

#include "media/filters/record_tap.h"
#include "media/transcode/ffmpeg_encode_muxer.h"
#include "media/base/decoder_buffer.h"
#include "base/threading/thread.h"
#include "player/public/player.h"
#include "player/public/player_config.h"

namespace avbase {

using PlayerStatus = media::Status;

// The transcode entry on the Player API surface — opt-in by link line
// (avbase_player_transcode). The Player facade itself deliberately carries
// no transcode: encoders and the mux writer live in avbase::transcode, a
// separate target, so a playback-only build does not pay for them
// (media/CMakeLists.txt, decision D11). Hosts that DO transcode link this
// target and get an entry that derives its defaults from a PlayerConfig,
// keeping one configuration surface for both playback and offline jobs.
//
// Threading mirrors media::Transcode: the synchronous call blocks the
// caller's thread; PlayerTranscoder owns a worker and never blocks Start().

// Synchronous transcode with PlayerConfig-derived defaults applied on top
// of |params|: config.video.disabled / config.audio.disabled (ijkplayer's
// -vn / -an) drop the corresponding output stream, exactly as they silence
// the playback legs. Everything else passes through to media::Transcode
// verbatim. |progress| runs on the calling thread; |cancel| is polled at
// packet boundaries.
PlayerStatus
TranscodeMedia(const std::string& input_uri,
               const media::TranscodeParams& params,
               media::TranscodeProgressCB progress = nullptr,
               const media::TranscodeCancelToken* cancel = nullptr);

// Playback-side recording, v2: a PACKET TAP on the playing pipeline --
// "record what the viewer actually sees". The tap sits where decoded-stream
// packets leave the demuxer for the decoders, so a seek of the player
// redirects the record (the post-seek generation flows in), a track switch
// switches what is recorded, and end of playback finalizes the file. Copy
// stays copy: tap packets are the compressed payloads, no decode/encode
// happens unless the caller asks (not wired -- copy covers the container the
// source already is).
//
// Segment semantics: a seek makes the demuxer timestamps jump backwards; the
// recorder re-bases each such segment to continue from the previous one, so
// the output is one continuous timeline of what was watched.
//
// The -vn/-an config flags strip streams; |overrides| audio/video codec
// names ("copy" etc.) win per-stream when non-empty. Requires a source whose
// URI the transcode-style demuxer can reopen? NO -- the tap needs no source
// access at all: packets arrive from the playing pipeline. Memory and
// descriptor sources record fine.
class PlayerRecorder {
 public:
  // Borrows |player| (Start/Stop mutate its tap state); the player must
  // outlive the recorder. |config| is the same PlayerConfig the player was
  // built with (its disabled-stream flags apply).
  PlayerRecorder(Player& player, const PlayerConfig& config);
  PlayerRecorder(const PlayerRecorder&) = delete;
  PlayerRecorder& operator=(const PlayerRecorder&) = delete;
  ~PlayerRecorder();

  // Starts recording |output_path| from this moment. The player should be
  // in kStarted (a record of a paused player holds the last frame only).
  // Returns an error when a record is already running or the state does not
  // allow it.
  Status Start(const std::string& output_path);

  // Detaches the tap and finalizes the container. Returns the recorded
  // status (kCancelled if the presentation was still running; that is the
  // normal Stop-during-playback outcome).
  Status Stop();

  bool IsRecording() const;

 private:
  // Taps (S3/S4): marshal onto the record thread. |video| binds the leg.
  void OnTapPacket(bool video,
                   base::scoped_refptr<media::DecoderBuffer> buffer);
  // Record thread: decoder-open header and packet writes.
  void OnTapStream(const media::RecordStreamHeader& header);

  Player& player_;
  PlayerConfig config_;
  media::RecordTap tap_;
  std::unique_ptr<base::Thread> record_thread_;
  base::scoped_refptr<base::SequencedTaskRunner> record_runner_;
  std::unique_ptr<media::FFmpegEncodeMuxer> muxer_;
  int audio_stream_ = -1;
  int video_stream_ = -1;
  int64_t audio_rate_ = 0;
  int64_t audio_last_pts_ = media::kNoPtsValue;
  int64_t video_last_pts_ = media::kNoPtsValue;
  int64_t audio_shift_ = 0;
  int64_t video_shift_ = 0;
  bool audio_ended_ = false;
  bool video_ended_ = false;
  bool finished_ = false;
  std::atomic<bool> recording_{false};
};

// Asynchronous handle: media::TranscodeJob pre-bound to the same
// PlayerConfig-derived defaults (-vn/-an). Start/Cancel/Wait/IsRunning
// forward to the job verbatim -- see ffmpeg_transcode_job.h for the
// threading contract.
class PlayerTranscoder {
 public:
  explicit PlayerTranscoder(const PlayerConfig& config);
  PlayerTranscoder(const PlayerTranscoder&) = delete;
  PlayerTranscoder& operator=(const PlayerTranscoder&) = delete;
  ~PlayerTranscoder();

  bool Start(const std::string& input_uri, const media::TranscodeParams& params,
             media::TranscodeProgressCB progress = nullptr,
             media::TranscodeDoneCB done_cb = nullptr);
  void Cancel();
  void Wait();
  bool IsRunning() const;

 private:
  media::TranscodeParams
  ApplyDefaults(const media::TranscodeParams& params) const;

  PlayerConfig config_;
  std::unique_ptr<media::TranscodeJob> job_;
};

}  // namespace avbase

#endif  // AVBASE_PLAYER_PUBLIC_PLAYER_TRANSCODE_H_

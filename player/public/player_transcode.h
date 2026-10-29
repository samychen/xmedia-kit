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

// Playback-side recording, v1 semantics. Start() runs a media::TranscodeJob
// over the player's CURRENT SOURCE beginning at the CURRENT media time:
// "record what is playing, linearly, from now on". It is NOT a surface tap:
// a seek of the player does not redirect the record (the job reads the source
// linearly), a volume/mute change is not reflected, and a paused player does
// not pause the record. The render-tap version (frames off the compositor +
// the audio ring) is the follow-up when those semantics are required.
//
// Defaults mirror what is playing: stream copy for both legs (fast, lossless
// for mp4->mp4 sources); |overrides| wins per-field when a field is set.
// The -vn/-an config flags still strip streams.
class PlayerRecorder {
 public:
  // Borrows |player|; the player must outlive the recorder. |config| is the
  // same PlayerConfig the player was built with (its disabled-stream flags
  // apply).
  PlayerRecorder(const Player& player, const PlayerConfig& config);
  PlayerRecorder(const PlayerRecorder&) = delete;
  PlayerRecorder& operator=(const PlayerRecorder&) = delete;
  ~PlayerRecorder();

  // Starts recording |output_path| from the current media time. The player
  // must be in kStarted or kPaused (a playhead exists). Returns an error
  // when the state does not allow it, when the source carries no URI, or
  // when a record is already running.
  Status Start(const std::string& output_path,
               const media::TranscodeParams& overrides = {});

  // Cancels the running job and waits for the output to be finalized.
  // Returns the job's final status (OkStatus on a clean stop; kCancelled
  // propagates as the job reports it).
  Status Stop();

  bool IsRecording() const;

 private:
  media::TranscodeParams BuildParams(
      const std::string& output_path,
      const media::TranscodeParams& overrides) const;

  const Player& player_;
  PlayerConfig config_;
  std::unique_ptr<media::TranscodeJob> job_;
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

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

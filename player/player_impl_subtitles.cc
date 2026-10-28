// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// External subtitles (cicada-port T6, the addExtSubtitle /
// selectExtSubtitle / setStreamDelay trio): registration, selection and
// cue emission for text tracks that do not come from the demuxer. Split
// from player_impl.cc for the C1 line limit; the cue event rides the same
// kTimedText payload the embedded text leg uses, so hosts see one shape.

#include "player/player_impl.h"

#include <cstdio>

#include "base/functional/bind.h"
#include "media/filters/external_text_track.h"
#include "player/public/player_config.h"

namespace avbase {
namespace {

// Loads a local subtitle file. External subtitles are local in every
// deployment this covers (CicadaPlayer's addExtSubtitle loads local files
// too); an http uri is an error with a suggestion, not a silent hang on
// the caller's thread.
Result<std::string> LoadLocalTextFile(const std::string& uri) {
  std::string path = uri;
  if (path.starts_with("file://")) {
    path.erase(0, 7);
  }
  FILE* file = fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return base::unexpected(MediaError::Of(
        ErrorCode::kSourceNotFound, "the subtitle file could not be opened",
        "uri = " + uri,
        "check the path; external subtitles load from local files only in "
        "this build"));
  }
  std::string text;
  char buffer[8192];
  size_t got = 0;
  while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
    text.append(buffer, got);
  }
  fclose(file);
  if (text.empty()) {
    return base::unexpected(MediaError::Of(
        ErrorCode::kSourceUnsupported, "the subtitle file is empty",
        "uri = " + uri, "check that the file holds srt or webvtt text"));
  }
  return text;
}

bool HasVttExtension(const std::string& uri) {
  return uri.ends_with(".vtt") || uri.ends_with(".webvtt");
}

}  // namespace

Result<int> PlayerImpl::AddExternalSubtitle(std::string_view uri,
                                            base::TimeDelta delay) {
  if (uri.empty()) {
    return base::unexpected(
        MediaError::Of(ErrorCode::kInvalidArgument, "the subtitle uri is empty",
                       "uri = (empty)",
                       "pass a local .srt or .vtt file path (or file:// url)"));
  }
  auto text = LoadLocalTextFile(std::string(uri));
  if (!text.has_value()) {
    return base::unexpected(text.error());
  }
  base::AutoLock scoped(state_lock_);
  const int track_id = next_external_track_id_++;
  auto track =
      media::ExternalTextTrack::Parse(*text, std::string(uri), track_id);
  if (!track.has_value()) {
    return base::unexpected(track.error());
  }
  track->SetDelay(delay);
  track->SetSelected(false);  // selection is explicit (selectExtSubtitle)
  external_tracks_.push_back(std::move(*track));
  // Start the cue ticker once; it runs until the player is destroyed.
  if (!external_text_ticking_) {
    external_text_ticking_ = true;
    external_text_last_tick_ = GetMediaTime();
    media_thread_.task_runner()->PostTask(
        FROM_HERE,
        base::BindOnce(&PlayerImpl::ExternalTextTick, base::Unretained(this)));
  }
  return track_id;
}

Status PlayerImpl::SelectExternalSubtitle(int track_id, bool selected) {
  base::AutoLock scoped(state_lock_);
  for (auto& track : external_tracks_) {
    if (track.track_id() == track_id) {
      track.SetSelected(selected);
      return OkStatus();
    }
  }
  return base::unexpected(MediaError::Of(
      ErrorCode::kInvalidArgument, "no such external subtitle track",
      "track_id = " + std::to_string(track_id) +
          ", tracks = " + std::to_string(external_tracks_.size()),
      "use the id AddExternalSubtitle returned"));
}

Status PlayerImpl::SetExternalSubtitleDelay(int track_id,
                                            base::TimeDelta delay) {
  base::AutoLock scoped(state_lock_);
  for (auto& track : external_tracks_) {
    if (track.track_id() == track_id) {
      track.SetDelay(delay);
      return OkStatus();
    }
  }
  return base::unexpected(MediaError::Of(
      ErrorCode::kInvalidArgument, "no such external subtitle track",
      "track_id = " + std::to_string(track_id),
      "use the id AddExternalSubtitle returned"));
}

Status PlayerImpl::RemoveExternalSubtitle(int track_id) {
  base::AutoLock scoped(state_lock_);
  for (auto it = external_tracks_.begin(); it != external_tracks_.end(); ++it) {
    if (it->track_id() == track_id) {
      external_tracks_.erase(it);
      return OkStatus();
    }
  }
  return base::unexpected(MediaError::Of(
      ErrorCode::kInvalidArgument, "no such external subtitle track",
      "track_id = " + std::to_string(track_id),
      "use the id AddExternalSubtitle returned"));
}

void PlayerImpl::ExternalTextTick() {
  if (!external_text_ticking_) {
    return;
  }
  PlayerState state;
  base::TimeDelta now;
  std::vector<media::TimedTextCue> due;
  {
    base::AutoLock scoped(state_lock_);
    state = machine_.state();
    now = GetMediaTime();
    // Cues are delivered for the (last, now] window only while the clock
    // actually advances; a pause, seek or completion holds the window so a
    // resumed playback does not replay the whole file.
    if (state == PlayerState::kStarted && now > external_text_last_tick_) {
      for (auto& track : external_tracks_) {
        auto cues = track.CuesBetween(external_text_last_tick_, now);
        due.insert(due.end(), cues.begin(), cues.end());
      }
    }
    external_text_last_tick_ = now;
  }
  for (const media::TimedTextCue& cue : due) {
    OnTimedText(cue);
  }
  media_thread_.task_runner()->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(&PlayerImpl::ExternalTextTick, base::Unretained(this)),
      base::Milliseconds(100));
}

}  // namespace avbase

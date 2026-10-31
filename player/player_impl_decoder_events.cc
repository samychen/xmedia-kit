// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The decoder-attribution events: which decoder each leg opened, and when one
// was replaced. Split out of player_impl_events.cc for the 500-line ceiling
// (C1), along the seam the feature already had: everything here is about one
// question ("what is decoding this stream") and shares no state with the
// buffering/statistics/seek events that file carries.
//
// The chain these two complete:
//   DecoderStream -> Video/AudioRendererImpl ->
//   RendererImpl::ForwardDecoderEvent
//   -> PipelineImpl -> PlayerImpl (here) -> EventHub -> host
//
// Before this existed the decoder's name was written to a LOG(INFO) line and
// dropped, so a host could not say "playing with VideoToolbox", could not tell
// whether frames were already in device memory, and could not see a hardware
// to software fallback at all.

#include "player/player_impl.h"

#include <string>
#include <utility>

#include "media/base/media_constants.h"
#include "player/public/player_event.h"

namespace xmedia {

void PlayerImpl::OnDecoderOpened(media::DemuxerStreamType type,
                                 const std::string& decoder_name,
                                 bool outputs_opaque_surface) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(buffer_controller_sequence_);
  // Kept so a host that asks LATE (after playback started, which is the normal
  // case for a status bar) still gets an answer: the event is a notification,
  // the field is the durable copy.
  {
    base::AutoLock scoped(snapshot_lock_);
    if (type == media::DemuxerStreamType::kVideo) {
      video_decoder_name_ = decoder_name;
      video_decoder_opaque_ = outputs_opaque_surface;
    } else if (type == media::DemuxerStreamType::kAudio) {
      audio_decoder_name_ = decoder_name;
    }
  }
  DecoderOpenedPayload payload;
  payload.type = type;
  payload.decoder_name = decoder_name;
  payload.outputs_opaque_surface = outputs_opaque_surface;
  event_hub_.Post(EventType::kDecoderOpened, std::move(payload),
                  GetMediaTime());
}

void PlayerImpl::OnDecoderFallback(media::DemuxerStreamType type,
                                   const std::string& from_decoder,
                                   const std::string& to_decoder,
                                   const std::string& reason) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(buffer_controller_sequence_);
  {
    base::AutoLock scoped(snapshot_lock_);
    // The opaque flag can change with the decoder (hardware -> software takes
    // frames out of device memory); it is re-read on the next kDecoderOpened
    // rather than guessed here.
    if (type == media::DemuxerStreamType::kVideo) {
      ++decoder_fallback_count_;
    }
  }
  DecoderFallbackPayload payload;
  payload.from_decoder = from_decoder;
  payload.to_decoder = to_decoder;
  payload.reason = reason;
  event_hub_.Post(EventType::kDecoderFallback, std::move(payload),
                  GetMediaTime());
}

void PlayerImpl::OnUtcAnchor(base::TimeDelta media_time, int64_t epoch_ms) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(buffer_controller_sequence_);
  {
    base::AutoLock scoped(snapshot_lock_);
    has_utc_anchor_ = true;
    utc_anchor_media_us_ = media_time.InMicroseconds();
    utc_anchor_epoch_ms_ = epoch_ms;
  }
  UtcTimePayload payload;
  payload.epoch_ms = epoch_ms;
  payload.anchor_media_time_us = media_time.InMicroseconds();
  event_hub_.Post(EventType::kUtcTime, std::move(payload), GetMediaTime());
}

Result<int64_t> PlayerImpl::GetUtcTimeMs() const {
  int64_t anchor_media_us = 0;
  int64_t anchor_epoch_ms = 0;
  {
    base::AutoLock scoped(snapshot_lock_);
    if (!has_utc_anchor_) {
      return base::unexpected(MediaError::Of(
          ErrorCode::kInvalidState, "the source carries no utc anchor",
          "only sources with a wall-clock mapping answer this (HLS "
          "EXT-X-PROGRAM-DATE-TIME today)",
          "check IsLive and the playlist's tags, or fall back to wall clock"));
    }
    anchor_media_us = utc_anchor_media_us_;
    anchor_epoch_ms = utc_anchor_epoch_ms_;
  }
  const base::TimeDelta now = GetMediaTime();
  if (now == media::kNoTimestamp) {
    return base::unexpected(MediaError::Of(
        ErrorCode::kInvalidState, "the media clock has not started",
        "GetMediaTime answered kNoTimestamp",
        "query again once playback started"));
  }
  return anchor_epoch_ms + (now.InMicroseconds() - anchor_media_us) / 1000;
}

std::string PlayerImpl::video_decoder_name() const {
  base::AutoLock scoped(snapshot_lock_);
  return video_decoder_name_;
}

std::string PlayerImpl::audio_decoder_name() const {
  base::AutoLock scoped(snapshot_lock_);
  return audio_decoder_name_;
}

bool PlayerImpl::video_decoder_opaque() const {
  base::AutoLock scoped(snapshot_lock_);
  return video_decoder_opaque_;
}

int PlayerImpl::decoder_fallback_count() const {
  base::AutoLock scoped(snapshot_lock_);
  return decoder_fallback_count_;
}

}  // namespace xmedia

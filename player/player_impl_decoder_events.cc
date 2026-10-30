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

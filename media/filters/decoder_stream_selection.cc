// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Decoder selection: which candidate a stream opens, and what it reports when
// one fails. Split from decoder_stream.cc for the 500-line ceiling (C1) along
// the seam the code already had -- these three functions own factories_ and
// active_factory_, and nothing in the read/decode pump touches either.
//
// The two events they raise are what make a host able to say which decoder is
// in use (kDecoderOpened) and to notice a hardware-to-software switch
// (kDecoderFallbackOnInit). Both were silent log lines before.

#include "media/filters/decoder_stream.h"

#include <string>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "media/base/decoder_buffer.h"

namespace xmedia::media {

template <typename Traits>
void DecoderStream<Traits>::Initialize(
    DemuxerStream* demuxer_stream, const ConfigType& config,
    std::vector<base::scoped_refptr<FactoryType>> factories, InitCB init_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  demuxer_stream_ = demuxer_stream;
  config_ = config;
  if (has_record_tap_ && record_tap_.on_stream) {
    RecordStreamHeader header;
    header.video = Traits::kIsVideo;
    header.codec_name = config.codec_name;
    if constexpr (Traits::kIsVideo) {
      header.width = config.coded_size.width;
      header.height = config.coded_size.height;
    } else {
      header.sample_rate = config.sample_rate;
      header.channels = config.channels;
    }
    header.extra_data = config.extra_data;
    record_tap_.on_stream(header);
  }
  factories_ = std::move(factories);
  next_factory_ = 0;
  TryNextFactory(std::move(init_cb));
}

template <typename Traits>
void DecoderStream<Traits>::TryNextFactory(InitCB init_cb) {
  if (next_factory_ >= factories_.size()) {
    Notify(DecoderStreamEvent::kNoDecoderAvailable);
    init_status_ = DecoderStatus(DecoderStatus::Codes::kUnknownError,
                                 std::string(Traits::name()) + ": all " +
                                     std::to_string(factories_.size()) +
                                     " decoder factories declined");
    LOG(WARNING) << "[decoder-stream] " << init_status_.description();
    std::move(init_cb).Run(init_status_);
    return;
  }
  base::scoped_refptr<FactoryType> factory = factories_[next_factory_++];
  std::unique_ptr<DecoderType> decoder =
      Traits::CreateDecoder(factory.get(), config_);
  if (!decoder) {
    // Not an error, just a "not me" from the factory -- move on.
    LOG(INFO) << "[decoder-stream] factory '" << factory->name()
              << "' declined the config";
    TryNextFactory(std::move(init_cb));
    return;
  }
  decoder_ = std::move(decoder);
  // The tap fires per decoder OPEN (a track switch re-fires with the new
  // stream's config) -- see record_tap.h.
  // Remembered before initialize: the failure path still asks whether the
  // abandoned candidate would have produced device memory.
  active_factory_ = std::move(factory);
  Traits::InitializeDecoder(
      decoder_.get(), config_, serial_,
      base::BindOnce(&DecoderStream::OnDecoderInitialized,
                     weak_factory_.GetWeakPtr(), std::move(init_cb)),
      base::BindRepeating(&DecoderStream::OnDecoderOutput,
                          weak_factory_.GetWeakPtr()),
      // Waiting is informational here: the DecoderStream's own back-pressure is
      // driven by Read(), not by decoder stalls.
      WaitingCB());
}

template <typename Traits>
void DecoderStream<Traits>::OnDecoderInitialized(InitCB init_cb,
                                                 DecoderStatus status) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!status.is_ok()) {
    LOG(WARNING) << "[decoder-stream] " << GetDisplayName()
                 << " failed to initialize: " << status.AsDebugString();
    // Captured before reset(): the owner's fallback report names the decoder
    // that lost. If no candidate succeeds, kNoDecoderAvailable reports instead.
    failed_decoder_ = Traits::DecoderName(decoder_.get());
    decoder_.reset();
    // A decoder we created and then lost is worth surfacing (Δ12); a factory
    // that returned nullptr is ordinary negotiation, reported nowhere.
    Notify(DecoderStreamEvent::kDecoderFallbackOnInit);
    ++fallbacks_;
    // Another candidate may still succeed, so keep going rather than failing.
    TryNextFactory(std::move(init_cb));
    return;
  }
  init_status_ = status;
  LOG(INFO) << "[decoder-stream] using " << GetDisplayName();
  failed_decoder_.clear();
  Notify(DecoderStreamEvent::kDecoderOpened);
  std::move(init_cb).Run(status);
}

// Explicit instantiations, repeated here because these three functions are
// now defined in this TU: without them the linker sees declarations only and
// every caller gets an undefined symbol (which is exactly what happened when
// the split was first made).
template class DecoderStream<VideoDecoderStreamTraits>;
template class DecoderStream<AudioDecoderStreamTraits>;

}  // namespace xmedia::media

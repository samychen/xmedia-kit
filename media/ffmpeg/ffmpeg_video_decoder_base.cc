// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/ffmpeg_video_decoder_base.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "media/base/decoder_status.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/av_packet_storage.h"
#include "media/ffmpeg/compat.h"

namespace xmedia::media {

namespace ff = ::xmedia::media::ffmpeg;

FFmpegVideoDecoderBase::FFmpegVideoDecoderBase(
    base::scoped_refptr<base::SequencedTaskRunner> task_runner,
    const char* decoder_name)
    : task_runner_(std::move(task_runner)),
      pending_decode_cbs_(task_runner_),
      decoder_name_(decoder_name) {
  CHECK(task_runner_) << decoder_name_
                      << " needs a task runner so that decode_cb is never run "
                         "inline";
  DETACH_FROM_SEQUENCE(sequence_checker_);
}

void FFmpegVideoDecoderBase::Decode(base::scoped_refptr<DecoderBuffer> buffer,
                                    DecodeCB decode_cb) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!decode_cb) {
    return;
  }
  AVCodecContext* ctx = codec_ctx();
  if (!initialized_ || !ctx) {
    task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(decode_cb),
                       DecoderStatus(DecoderStatus::Codes::kNotInitialized,
                                     "Initialize() did not succeed")));
    return;
  }
  pending_decode_cbs_.Push(std::move(decode_cb));

  if (!buffer) {
    pending_decode_cbs_.RunOne(
        DecoderStatus(DecoderStatus::Codes::kDecodeError, "null buffer"));
    return;
  }

  if (buffer->IsEndOfStream()) {
    // EOS: flush the decoder, emit everything still buffered, then answer every
    // outstanding request. avcodec_send_packet(nullptr) is the documented
    // flush. There is no EOS flag to keep on the side: a repeated EOS is
    // harmless because it only re-sends the flush, and both decoders treat the
    // second one as "already drained" -- avcodec_receive_frame returns
    // AVERROR_EOF.
    avcodec_send_packet(ctx, nullptr);
    if (!DecodeAvailableFrames()) {
      // RunAll, not RunOne: a failed flush leaves the decoder unusable, so a
      // second waiter would never hear back.
      pending_decode_cbs_.RunAll(
          DecoderStatus(DecoderStatus::Codes::kDecodeError,
                        std::string(decoder_name_) + ": flush failed"));
      return;
    }
    pending_decode_cbs_.RunAll(DecoderStatus());
    return;
  }

  auto* storage = buffer->storage_as<ff::AvPacketStorage>();
  if (!storage || !storage->raw()) {
    // A buffer that did not come from the FFmpeg demuxer. This is the type
    // erasure boundary failing loudly rather than silently mis-decoding.
    pending_decode_cbs_.RunOne(DecoderStatus(
        DecoderStatus::Codes::kDecodeError,
        std::string("buffer storage is not an AVPacket; a custom Demuxer must "
                    "produce AvPacketStorage for ") +
            decoder_name_));
    return;
  }

  current_serial_ = buffer->serial();
  // avcodec_send_packet consumes the packet's reference; the DecoderBuffer
  // keeps its own, so the buffer stays valid for diagnostics and for a possible
  // retry.
  const int send_ret = avcodec_send_packet(ctx, storage->raw());
  if (send_ret < 0 && send_ret != AVERROR(EAGAIN)) {
    pending_decode_cbs_.RunOne(
        ff::ToDecoderStatus(send_ret, "avcodec_send_packet"));
    return;
  }
  if (!DecodeAvailableFrames()) {
    pending_decode_cbs_.RunOne(DecoderStatus(
        DecoderStatus::Codes::kDecodeError,
        std::string(decoder_name_) + ": avcodec_receive_frame failed"));
    return;
  }
  pending_decode_cbs_.RunOne(DecoderStatus());
}

void FFmpegVideoDecoderBase::Reset(base::OnceClosure closure) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (AVCodecContext* ctx = codec_ctx()) {
    avcodec_flush_buffers(ctx);
  }
  OnResetExtra();
  // Every outstanding request is answered before |closure|, per the contract.
  pending_decode_cbs_.RunAll(
      DecoderStatus(DecoderStatus::Codes::kDecodingAborted, "Reset()"));
  if (closure) {
    task_runner_->PostTask(FROM_HERE, std::move(closure));
  }
}

int FFmpegVideoDecoderBase::GetMaxDecodeRequests() const {
  // Decoding completes inside Decode(); allowing more in flight would only
  // reorder outputs.
  return 1;
}

}  // namespace xmedia::media

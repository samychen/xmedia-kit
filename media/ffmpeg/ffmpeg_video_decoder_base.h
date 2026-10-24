// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FFMPEG_FFMPEG_VIDEO_DECODER_BASE_H_
#define AVBASE_MEDIA_FFMPEG_FFMPEG_VIDEO_DECODER_BASE_H_

#include <cstdint>

#include "base/memory/scoped_refptr.h"
#include "base/sequence_checker.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/decoder_buffer.h"
#include "media/base/video_decoder.h"
#include "media/ffmpeg/pending_decode_callbacks.h"
#include "media/media_export.h"

// Forward declaration only: this header is included by the per-platform hw
// factories (platform/videotoolbox, platform/vaapi, platform/d3d11), which must
// not pull in a libav header. `struct AVCodecContext` has a tag name in
// avcodec.h, so naming it here does not require the definition.
struct AVCodecContext;

namespace avbase::media {

// The Decode()/Reset() skeleton shared by FFmpegVideoDecoder (software) and
// FFmpegHwVideoDecoder (hardware).
//
// Both paths feed an AVCodecContext identically -- reject a call before a
// successful Initialize(), validate the buffer, hand the AVPacket to
// libavcodec, drain the frames it produced, then answer exactly one outstanding
// decode_cb -- and differ only in how a frame is produced (swscale vs a
// hardware transfer) and in one piece of EOS bookkeeping. Keeping the sequence
// here means the two paths cannot drift apart again; they already had: on a
// failed EOS flush the hardware path answered one pending callback while the
// software path answered all of them, so any extra waiter on the hw path would
// have hung forever.
//
// A subclass supplies four things:
//   * codec_ctx()             -- its own (opaque) codec context
//   * DecodeAvailableFrames() -- drain avcodec_receive_frame, convert, emit
//   * OnEndOfStream()         -- optional EOS bookkeeping
//   * OnResetExtra()          -- optional per-path state to drop on Reset()
class AVBASE_MEDIA_EXPORT FFmpegVideoDecoderBase : public VideoDecoder {
 public:
  // VideoDecoder, implemented once for both paths.
  void Decode(base::scoped_refptr<DecoderBuffer> buffer,
              DecodeCB decode_cb) final;
  void Reset(base::OnceClosure closure) final;
  int GetMaxDecodeRequests() const final;

 protected:
  // |decoder_name| only names this decoder in error statuses, so a failure says
  // which path produced it.
  FFmpegVideoDecoderBase(
      base::scoped_refptr<base::SequencedTaskRunner> task_runner,
      const char* decoder_name);

  // The concrete decoder's codec context; null until Initialize() succeeds.
  virtual AVCodecContext* codec_ctx() const = 0;

  // Drains avcodec_receive_frame, converts and emits. Returns false on error.
  virtual bool DecodeAvailableFrames() = 0;

  // Called right after the EOS flush packet is sent and before draining. The
  // software decoder records the flush so a repeated EOS is a no-op; the
  // hardware path has nothing to record.
  virtual void OnEndOfStream() {}

  // Called from Reset() once the codec context is flushed, before the pending
  // callbacks are answered. The software decoder drops its converter and EOS
  // flag here; the hardware path has no extra state.
  virtual void OnResetExtra() {}

  // ---- state the skeleton owns -------------------------------------------
  base::scoped_refptr<base::SequencedTaskRunner> task_runner_;
  PendingDecodeCallbacks pending_decode_cbs_;
  OutputCB output_cb_;
  WaitingCB waiting_cb_;
  bool initialized_{false};
  int32_t current_serial_{0};
  uint64_t frames_decoded_{0};

  SEQUENCE_CHECKER(sequence_checker_);

 private:
  const char* const decoder_name_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FFMPEG_FFMPEG_VIDEO_DECODER_BASE_H_

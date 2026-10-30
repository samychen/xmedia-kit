// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Replaces ijkplayer's pipeline/ffpipenode_ffplay_vdec.c.

#ifndef XMEDIA_MEDIA_FFMPEG_VIDEO_DECODER_H_
#define XMEDIA_MEDIA_FFMPEG_VIDEO_DECODER_H_

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/video_decoder.h"
#include "media/ffmpeg/ffmpeg_video_decoder_base.h"
#include "media/media_export.h"

namespace xmedia::media {

// Software video decoding through libavcodec, with swscale conversion to the
// requested output format.
//
// Threading: all methods run on the sequence given at construction. FFmpeg's
// software decoders are synchronous internally (their own frame/slice threads
// are hidden behind avcodec_send_packet/receive_frame), so Decode() produces
// every available frame before returning — but decode_cb is still posted, never
// run inline, to honour the VideoDecoder contract.
//
// Decode()/Reset()/GetMaxDecodeRequests() come from FFmpegVideoDecoderBase,
// which holds the buffer-validation and callback-draining sequence shared with
// the hardware path. This class supplies the codec context, the swscale drain,
// and the two pieces of software-only EOS/Reset bookkeeping.
class XMEDIA_MEDIA_EXPORT FFmpegVideoDecoder final
    : public FFmpegVideoDecoderBase {
 public:
  // |task_runner| is where decode_cb and init_cb are posted. Required.
  explicit FFmpegVideoDecoder(
      base::scoped_refptr<base::SequencedTaskRunner> task_runner);
  FFmpegVideoDecoder(const FFmpegVideoDecoder&) = delete;
  FFmpegVideoDecoder& operator=(const FFmpegVideoDecoder&) = delete;
  ~FFmpegVideoDecoder() override;

  // VideoDecoder:
  void Initialize(const VideoDecoderConfig& config, bool low_delay,
                  CdmContext* cdm_context, InitCB init_cb,
                  const OutputCB& output_cb,
                  const WaitingCB& waiting_cb) override;
  VideoDecoderType GetDecoderType() const override {
    return VideoDecoderType::kFFmpegVideoDecoder;
  }
  const char* name() const override { return "FFmpegVideoDecoder"; }

  // Output format requested at construction time; kUnknown means "keep the
  // decoder's native format when it is already CPU-mappable".
  void set_preferred_output_format(VideoFormat format) {
    preferred_format_ = format;
  }
  void set_thread_count(int threads) { thread_count_ = threads; }

  // Diagnostics.
  uint64_t frames_decoded() const { return frames_decoded_; }
  uint64_t conversion_failures() const { return conversion_failures_; }

 private:
  // Opaque FFmpeg handles; kept out of the header.
  struct Context;

  DecoderStatus OpenCodec(const VideoDecoderConfig& config, int threads);

  // FFmpegVideoDecoderBase:
  AVCodecContext* codec_ctx() const override;
  bool DecodeAvailableFrames() override;
  void OnResetExtra() override;

  std::unique_ptr<Context> ctx_;
  VideoDecoderConfig config_;
  VideoFormat preferred_format_{VideoFormat::kUnknown};
  int thread_count_{0};
  uint64_t conversion_failures_{0};
};

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_FFMPEG_VIDEO_DECODER_H_

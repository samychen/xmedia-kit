// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/ffmpeg_video_decoder.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "media/base/media_constants.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/av_packet_storage.h"
#include "media/ffmpeg/color_space_bridge.h"
#include "media/ffmpeg/compat.h"
#include "media/ffmpeg/ffmpeg_util.h"
#include "media/ffmpeg/video_convert.h"

namespace avbase::media {
namespace {

namespace ff = ::avbase::media::ffmpeg;

// Keeps the decoder's own format when it is already CPU-mappable, so the common
// case (yuv420p source, no requested override) needs no swscale at all.
bool NeedsConversion(AVPixelFormat src, VideoFormat dst) {
  return ff::ToAvPixelFormat(dst) != src;
}

}  // namespace

struct FFmpegVideoDecoder::Context {
  ff::CodecCtxPtr codec_ctx;
  ff::FramePtr frame;
  ff::VideoConverter converter;
};

FFmpegVideoDecoder::FFmpegVideoDecoder(
    base::scoped_refptr<base::SequencedTaskRunner> task_runner)
    : FFmpegVideoDecoderBase(std::move(task_runner), "FFmpegVideoDecoder"),
      ctx_(std::make_unique<Context>()) {}

FFmpegVideoDecoder::~FFmpegVideoDecoder() = default;

AVCodecContext* FFmpegVideoDecoder::codec_ctx() const {
  return ctx_->codec_ctx.get();
}

void FFmpegVideoDecoder::OnResetExtra() {
  ctx_->converter.Reset();
}

DecoderStatus FFmpegVideoDecoder::OpenCodec(const VideoDecoderConfig& config,
                                            int threads) {
  const AVCodec* codec =
      avcodec_find_decoder_by_name(config.codec_name.c_str());
  if (!codec) {
    return DecoderStatus(DecoderStatus::Codes::kUnsupportedCodec,
                         "no decoder named \"" + config.codec_name + "\"");
  }
  ff::CodecCtxPtr codec_ctx(avcodec_alloc_context3(codec));
  if (!codec_ctx) {
    return DecoderStatus(DecoderStatus::Codes::kUnknownError,
                         "avcodec_alloc_context3 failed");
  }
  codec_ctx->width = config.coded_size.width;
  codec_ctx->height = config.coded_size.height;
  // Without pkt_timebase, AVFrame::pts stays in an undefined time base and
  // av_rescale_q() returns 0 for every frame — which shows up downstream as
  // "all frames have timestamp 0" and destroys A/V sync. It must be the
  // container stream's time base, carried through VideoDecoderConfig.
  codec_ctx->pkt_timebase =
      AVRational{config.time_base.num, config.time_base.den};
  if (codec_ctx->pkt_timebase.num <= 0 || codec_ctx->pkt_timebase.den <= 0) {
    codec_ctx->pkt_timebase = AVRational{1, AV_TIME_BASE};
  }
  codec_ctx->workaround_bugs = FF_BUG_AUTODETECT;
  codec_ctx->err_recognition = 0;
  if (threads > 0) {
    codec_ctx->thread_count = threads;
    // FF_THREAD_SLICE gives lower latency than FF_THREAD_FRAME, which matters
    // for the first frame; frame threading wins on throughput once running.
    codec_ctx->thread_type = FF_THREAD_SLICE | FF_THREAD_FRAME;
  }
  if (!config.extra_data.empty()) {
    codec_ctx->extradata = static_cast<uint8_t*>(
        av_mallocz(config.extra_data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!codec_ctx->extradata) {
      return DecoderStatus(DecoderStatus::Codes::kUnknownError,
                           "extradata allocation failed");
    }
    memcpy(codec_ctx->extradata, config.extra_data.data(),
           config.extra_data.size());
    codec_ctx->extradata_size = static_cast<int>(config.extra_data.size());
  }

  const int ret = avcodec_open2(codec_ctx.get(), codec, nullptr);
  if (ret < 0) {
    return DecoderStatus(DecoderStatus::Codes::kDecodeError,
                         "avcodec_open2: " + ff::AvErrorString(ret));
  }
  ctx_->codec_ctx = std::move(codec_ctx);
  // ff::MakeFrame CHECKs internally; there is no failure mode to report.
  ctx_->frame = ff::MakeFrame();
  return DecoderStatus();
}

void FFmpegVideoDecoder::Initialize(const VideoDecoderConfig& config,
                                    bool /*low_delay*/, CdmContext* /*cdm*/,
                                    InitCB init_cb, const OutputCB& output_cb,
                                    const WaitingCB& /*waiting_cb*/) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  output_cb_ = output_cb;
  config_ = config;
  ctx_ = std::make_unique<Context>();

  // Report the specific problem rather than a blanket "config is not valid":
  // the caller cannot act on a message that does not say which field is wrong.
  DecoderStatus status;
  if (config.codec == VideoCodec::kUnknown || config.codec_name.empty()) {
    status = DecoderStatus(DecoderStatus::Codes::kUnsupportedCodec,
                           "unknown video codec (codec_name=\"" +
                               config.codec_name + "\")");
  } else if (config.coded_size.IsEmpty()) {
    status =
        DecoderStatus(DecoderStatus::Codes::kUnsupportedResolution,
                      "coded_size is empty for codec " + config.codec_name);
  } else {
    status = OpenCodec(config, thread_count_);
  }
  initialized_ = status.is_ok();
  if (!initialized_) {
    LOG(ERROR) << "FFmpegVideoDecoder::Initialize failed: "
               << status.AsDebugString();
  }
  if (init_cb) {
    task_runner_->PostTask(FROM_HERE,
                           base::BindOnce(std::move(init_cb), status));
  }
}

bool FFmpegVideoDecoder::DecodeAvailableFrames() {
  AVCodecContext* codec_ctx = ctx_->codec_ctx.get();
  AVFrame* frame = ctx_->frame.get();
  for (;;) {
    const int ret = avcodec_receive_frame(codec_ctx, frame);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
      return true;  // Needs more input, or fully drained. Not an error.
    }
    if (ret < 0) {
      LOG(ERROR) << "avcodec_receive_frame: " << ff::AvErrorString(ret);
      return false;
    }

    const auto src_format = static_cast<AVPixelFormat>(frame->format);
    VideoFormat dst_format = preferred_format_;
    if (dst_format == VideoFormat::kUnknown) {
      dst_format = ff::FromAvPixelFormat(src_format, VideoFormat::kUnknown);
    }
    if (dst_format == VideoFormat::kUnknown) {
      dst_format = VideoFormat::kI420;  // Always convertible.
    }

    const int width = frame->width;
    const int height = frame->height;
    if (NeedsConversion(src_format, dst_format)) {
      // Configure() is a no-op while the geometry and formats are unchanged;
      // it rebuilds the fallback context only when they move.
      if (!ctx_->converter.Configure(width, height, src_format, width, height,
                                     ff::ToAvPixelFormat(dst_format))) {
        ++conversion_failures_;
        av_frame_unref(frame);
        return false;
      }
    } else {
      ctx_->converter.Reset();
    }

    Rational sar{1, 1};
    if (frame->sample_aspect_ratio.num > 0 &&
        frame->sample_aspect_ratio.den > 0) {
      sar = Rational{frame->sample_aspect_ratio.num,
                     frame->sample_aspect_ratio.den};
    } else if (config_.sar.num > 0) {
      sar = config_.sar;
    }
    auto out = VideoFrame::CreateBlackFrame(
        dst_format, Size{width, height}, Size{width, height}, sar,
        ff::ToTimeDelta(frame->pts, codec_ctx->pkt_timebase),
        ff::ToTimeDelta(frame->duration, codec_ctx->pkt_timebase),
        current_serial_);
    if (!out) {
      av_frame_unref(frame);
      return false;
    }
    out->set_color_space(ff::ColorSpaceFromAvFrame(frame, config_));
    // Carry the stream's display rotation onto the frame: it lives on the
    // config because that is what the container filled in, and it belongs on
    // the frame because the renderer is the only thing that can act on it.
    out->set_rotation(config_.rotation);

    if (ctx_->converter.Configured()) {
      uint8_t* dst[4] = {nullptr, nullptr, nullptr, nullptr};
      int dst_stride[4] = {0, 0, 0, 0};
      const int planes = VideoFormatPlaneCount(dst_format);
      for (int p = 0; p < planes && p < VideoFrame::kMaxPlanes; ++p) {
        const auto plane = static_cast<VideoFrame::Plane>(p);
        // mutable_data() is the producer-side accessor: |out| was just created
        // here and has exactly one owner, so writing it needs neither a cast
        // nor a lock. Consumers only ever see visible_data().
        dst[p] = out->mutable_data(plane).data();
        dst_stride[p] = out->stride(plane);
      }
      if (!ctx_->converter.Convert(*frame, dst, dst_stride)) {
        ++conversion_failures_;
        av_frame_unref(frame);
        continue;
      }
    } else {
      // No conversion needed: copy plane by plane. swscale would do the same
      // work with more overhead, and this path keeps the common yuv420p case
      // allocation-light.
      const int planes = VideoFormatPlaneCount(dst_format);
      for (int p = 0; p < planes && p < VideoFrame::kMaxPlanes; ++p) {
        const auto plane = static_cast<VideoFrame::Plane>(p);
        std::span<uint8_t> dst_span = out->mutable_data(plane);
        if (dst_span.empty() || !frame->data[p]) {
          continue;
        }
        const int src_stride = frame->linesize[p];
        const int dst_stride = out->stride(plane);
        const int row_bytes = std::min<int>(src_stride, dst_stride);
        const int rows = (p == 0) ? height : ((height + 1) / 2);
        for (int r = 0; r < rows; ++r) {
          memcpy(dst_span.data() +
                     static_cast<size_t>(r) * static_cast<size_t>(dst_stride),
                 frame->data[p] +
                     static_cast<size_t>(r) * static_cast<size_t>(src_stride),
                 static_cast<size_t>(row_bytes));
        }
      }
    }

    av_frame_unref(frame);
    ++frames_decoded_;
    if (output_cb_) {
      output_cb_.Run(std::move(out));
    }
  }
}

}  // namespace avbase::media

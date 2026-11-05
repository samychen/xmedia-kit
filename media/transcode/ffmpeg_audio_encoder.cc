// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/transcode/ffmpeg_audio_encoder.h"

#include <cstring>
#include <utility>

#include "base/check.h"
#include "base/logging.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/audio_convert.h"
#include "media/ffmpeg/compat.h"

namespace xmedia::media {
namespace {

namespace ff = ::xmedia::media::ffmpeg;

}  // namespace

struct FFmpegAudioEncoder::Context {
  AVCodecContext* codec = nullptr;
  int channels = 0;
  int64_t next_pts_samples = 0;
  // The codec's own sample format (negotiated at Initialize; FLTP whenever
  // the codec takes it). A format other than FLTP engages the conversion
  // leg in Encode(): the decode path hands over planar float buffers, and
  // e.g. pcm_s16le wants packed S16.
  AVSampleFormat codec_fmt = AV_SAMPLE_FMT_FLTP;
  std::unique_ptr<ffmpeg::AudioConverter> fmt_cvt;
  bool fmt_cvt_ready = false;

  Context() = default;
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
  // Same rule as FFmpegVideoEncoder::Context: a failed Initialize() used to
  // leak the context it had already allocated.
  ~Context() {
    if (codec) {
      avcodec_free_context(&codec);
    }
  }
};

FFmpegAudioEncoder::FFmpegAudioEncoder() = default;

FFmpegAudioEncoder::~FFmpegAudioEncoder() = default;

int FFmpegAudioEncoder::frame_size() const {
  return ctx_ && ctx_->codec ? ctx_->codec->frame_size : 0;
}

bool FFmpegAudioEncoder::Initialize(const Params& params) {
  ctx_.reset();
  const std::string name =
      params.codec_name.empty() ? std::string("aac") : params.codec_name;
  const AVCodec* codec = avcodec_find_encoder_by_name(name.c_str());
  if (!codec) {
    LOG(ERROR) << "audio encoder: no encoder named \"" << name
               << "\" in this FFmpeg build";
    return false;
  }
  auto c = std::make_unique<Context>();
  c->codec = avcodec_alloc_context3(codec);
  if (!c->codec) {
    return false;
  }
  c->channels = params.channels;
  c->codec->sample_rate = params.sample_rate;
  av_channel_layout_default(&c->codec->ch_layout, params.channels);
  // Sample-format negotiation. FLTP stays the default: it is what the
  // decode path produces and what every modern audio encoder (aac,
  // libmp3lame, libopus, ac3, flac) accepts. Encoders that take something
  // else (pcm_s16le is packed S16) get their first advertised format, and
  // Encode() converts into it -- the video encoder hardcodes YUV420P the
  // same way for the same reason.
  c->codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
  c->codec_fmt = AV_SAMPLE_FMT_FLTP;
  if (codec->sample_fmts != nullptr) {
    bool fltp_listed = false;
    for (int i = 0; codec->sample_fmts[i] != AV_SAMPLE_FMT_NONE; ++i) {
      if (codec->sample_fmts[i] == AV_SAMPLE_FMT_FLTP) {
        fltp_listed = true;
        break;
      }
    }
    if (!fltp_listed) {
      c->codec_fmt = codec->sample_fmts[0];
      c->codec->sample_fmt = c->codec_fmt;
    }
  }
  if (params.bit_rate > 0) {
    c->codec->bit_rate = params.bit_rate;
  }
  if (avcodec_open2(c->codec, codec, nullptr) < 0) {
    LOG(ERROR) << "audio encoder: avcodec_open2 failed for \"" << name << "\"";
    avcodec_free_context(&c->codec);
    return false;
  }
  if (c->codec->extradata && c->codec->extradata_size > 0) {
    ctx_extradata_.assign(c->codec->extradata,
                          c->codec->extradata + c->codec->extradata_size);
  }
  ctx_ = std::move(c);
  return true;
}

// The non-FLTP leg: view the planar-float buffer as an AVFrame, convert into
// the codec's format through the shared AudioConverter (rate/channels already
// match; only the format differs), and send each converted frame. PTS walks
// in output samples -- identical accounting to the FLTP path.
bool FFmpegAudioEncoder::EncodeConverted(base::scoped_refptr<AudioBuffer> in,
                                         std::vector<EncodedPacket>* out) {
  if (!ctx_->fmt_cvt) {
    ctx_->fmt_cvt = std::make_unique<ffmpeg::AudioConverter>();
  }
  if (!ctx_->fmt_cvt_ready) {
    if (!ctx_->fmt_cvt->Configure(
            ctx_->codec->sample_rate, ctx_->channels, AV_SAMPLE_FMT_FLTP,
            ctx_->codec->sample_rate, ctx_->channels, ctx_->codec_fmt)) {
      LOG(ERROR) << "audio encoder: format conversion setup failed ("
                 << av_get_sample_fmt_name(ctx_->codec_fmt) << ")";
      return false;
    }
    ctx_->fmt_cvt_ready = true;
  }
  ff::FramePtr src = ff::MakeFrame();
  src->format = AV_SAMPLE_FMT_FLTP;
  src->sample_rate = ctx_->codec->sample_rate;
  src->nb_samples = in->frame_count();
  av_channel_layout_copy(&src->ch_layout, &ctx_->codec->ch_layout);
  src->pts = ctx_next_pts_samples_;
  for (int ch = 0; ch < ctx_->channels; ++ch) {
    src->data[ch] = const_cast<uint8_t*>(in->channel_data(ch).data());
    src->linesize[ch] = static_cast<int>(in->channel_data(ch).size());
  }
  auto converted = ctx_->fmt_cvt->Push(*src);
  if (converted.empty()) {
    LOG(ERROR) << "audio encoder: format conversion produced no frames";
    return false;
  }
  for (auto& frame : converted) {
    frame->sample_rate = ctx_->codec->sample_rate;
    frame->pts = ctx_next_pts_samples_;
    ctx_next_pts_samples_ += frame->nb_samples;
    const int send = avcodec_send_frame(ctx_->codec, frame.get());
    if (send < 0) {
      char msg[AV_ERROR_MAX_STRING_SIZE] = {};
      LOG(ERROR) << "audio encoder: converted send_frame failed: "
                 << av_make_error_string(msg, sizeof(msg), send);
      return false;
    }
  }
  while (true) {
    ff::PacketPtr packet = ff::MakePacket();
    const int got = avcodec_receive_packet(ctx_->codec, packet.get());
    if (got < 0) {
      break;  // EAGAIN: needs more input.
    }
    EncodedPacket ep;
    ep.data.assign(packet->data, packet->data + packet->size);
    ep.pts = packet->pts;
    ep.dts = packet->dts;
    ep.duration = packet->duration;
    ep.flags = packet->flags;
    out->push_back(std::move(ep));
  }
  return true;
}

bool FFmpegAudioEncoder::Encode(base::scoped_refptr<AudioBuffer> in,
                                std::vector<EncodedPacket>* out) {
  if (!ctx_ || !out) {
    return false;
  }
  if (ctx_->codec_fmt != AV_SAMPLE_FMT_FLTP) {
    return EncodeConverted(std::move(in), out);
  }
  // M10's deeper fix: ff::MakeFrame CHECKs internally (this site keeps a raw
  // pointer for the unref below); there is no failure mode to report.
  AVFrame* frame = av_frame_alloc();
  CHECK(frame) << "xmedia.transcode: av_frame_alloc failed (OOM)";
  frame->format = ctx_->codec->sample_fmt;
  frame->sample_rate = ctx_->codec->sample_rate;
  frame->nb_samples = in->frame_count();
  av_channel_layout_copy(&frame->ch_layout, &ctx_->codec->ch_layout);
  frame->pts = ctx_next_pts_samples_;
  for (int ch = 0; ch < ctx_->channels; ++ch) {
    frame->data[ch] = const_cast<uint8_t*>(in->channel_data(ch).data());
  }
  const int send = avcodec_send_frame(ctx_->codec, frame);
  // Read everything the error path wants to report BEFORE freeing: av_frame
  // is gone after this line, and logging its fields later is a use-after-free
  // that crashes the very diagnostic meant to explain the failure.
  const int sent_nb_samples = frame->nb_samples;
  const int sent_rate = frame->sample_rate;
  const int sent_channels = frame->ch_layout.nb_channels;
  av_frame_free(&frame);
  if (send < 0) {
    char msg[AV_ERROR_MAX_STRING_SIZE] = {};
    LOG(ERROR) << "audio encoder: send_frame failed: "
               << av_make_error_string(msg, sizeof(msg), send)
               << " (nb_samples=" << sent_nb_samples << " rate=" << sent_rate
               << " ch=" << sent_channels << ")";
    return false;
  }
  ctx_next_pts_samples_ += in->frame_count();

  while (true) {
    ff::PacketPtr packet = ff::MakePacket();
    if (!packet) {
      // M10: OOM inside the drain loop used to null-deref in
      // avcodec_receive_packet.
      return false;
    }
    const int got = avcodec_receive_packet(ctx_->codec, packet.get());
    if (got < 0) {
      break;  // EAGAIN: needs more input.
    }
    EncodedPacket ep;
    ep.data.assign(packet->data, packet->data + packet->size);
    ep.pts = packet->pts;
    ep.dts = packet->dts;
    ep.duration = packet->duration;
    ep.flags = packet->flags;
    out->push_back(std::move(ep));
  }
  return true;
}

bool FFmpegAudioEncoder::Flush(std::vector<EncodedPacket>* out) {
  if (!ctx_ || !out) {
    return false;
  }
  if (avcodec_send_frame(ctx_->codec, nullptr) < 0) {
    return false;
  }
  while (true) {
    ff::PacketPtr packet = ff::MakePacket();
    if (!packet) {
      // M10: OOM inside the drain loop used to null-deref in
      // avcodec_receive_packet.
      return false;
    }
    const int got = avcodec_receive_packet(ctx_->codec, packet.get());
    if (got < 0) {
      break;
    }
    EncodedPacket ep;
    ep.data.assign(packet->data, packet->data + packet->size);
    ep.pts = packet->pts;
    ep.dts = packet->dts;
    ep.duration = packet->duration;
    ep.flags = packet->flags;
    out->push_back(std::move(ep));
  }
  return true;
}

}  // namespace xmedia::media

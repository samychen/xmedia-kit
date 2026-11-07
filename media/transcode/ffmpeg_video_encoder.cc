// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/transcode/ffmpeg_video_encoder.h"

#include <cstring>
#include <utility>

#include "base/logging.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"
#include "media/transcode/frame_hash.h"
#include "media/ffmpeg/ffmpeg_util.h"
#include "media/ffmpeg/gif_palette.h"
#include "media/ffmpeg/video_convert.h"

namespace xmedia::media {
namespace {

namespace ff = ::xmedia::media::ffmpeg;

}  // namespace

struct FFmpegVideoEncoder::Context {
  AVCodecContext* codec = nullptr;
  AVCodecParameters* par = nullptr;
  AVPixelFormat in_fmt = AV_PIX_FMT_YUV420P;   // Encoder's input format.
  AVPixelFormat dst_fmt = AV_PIX_FMT_YUV420P;  // Converted frame format.
  ff::VideoConverter converter;
  ff::FramePtr converted;
  // The GIF leg: the codec takes PAL8 only and swscale cannot output pal8,
  // so Encode() converts into RGBA, quantizes with the median-cut palette,
  // and sends a PAL8 frame. |rgba| is the quantizer's input; |palette| is
  // rebuilt per frame (GIF palettes are per-frame by spec).
  bool is_gif = false;
  bool is_hdr = false;
  ff::FramePtr rgba;
  ff::GifFramePalette palette;
  ff::FramePtr pal;
  // T9a static-frame dedup: the hash of the last input frame and the frames
  // sent to the encoder since then. A hash match skips conversion (sws /
  // libyuv / palette rebuild) and re-sends the cached output frame at the
  // new pts — the encoder still encodes it, but skips the expensive
  // per-frame pixel pipeline. Deterministic encoders (libx264, gif) produce
  // byte-identical output; non-deterministic ones produce PSNR-equivalent
  // output (same as MC's T9a finding).
  bool last_input_valid = false;
  uint64_t last_input_hash = 0;
  int64_t dedup_skipped = 0;

  Context() = default;
  Context(const Context&) = delete;
  Context& operator=(const Context&) = delete;
  // Owns the libav objects. Before this existed, every Initialize() that
  // returned false leaked its half-built context — which is exactly the
  // path a caller walks when it tries candidate encoders in order and the
  // first one is missing from the build.
  ~Context() {
    if (par) {
      avcodec_parameters_free(&par);
    }
    if (codec) {
      avcodec_free_context(&codec);
    }
  }

  // Drains queued packets from the encoder into EncodedPacket objects that
  // carry pts/dts/flags/duration and side_data (h264 SPS/PPS in-band, etc.)
  // so the muxer receives the same information av_interleaved_write_frame
  // would get from a raw AVPacket.
  void DrainPackets(std::vector<EncodedPacket>* out) {
    while (true) {
      ff::PacketPtr packet = ff::MakePacket();
      const int got = avcodec_receive_packet(codec, packet.get());
      if (got < 0) {
        break;
      }
      EncodedPacket ep;
      ep.data.assign(packet->data, packet->data + packet->size);
      ep.pts = packet->pts;
      ep.dts = packet->dts;
      ep.duration = packet->duration;
      ep.flags = packet->flags;
      // Carry side data through so h264's SPS/PPS (AV_PKT_DATA_NEW_EXTRADATA)
      // and other per-packet metadata survive into the muxer.
      for (int i = 0; i < packet->side_data_elems; ++i) {
        const auto& sd = packet->side_data[i];
        EncodedPacket::SideData side;
        side.type = sd.type;
        side.bytes.assign(sd.data, sd.data + sd.size);
        ep.side_data.push_back(std::move(side));
      }
      out->push_back(std::move(ep));
    }
  }
};

FFmpegVideoEncoder::FFmpegVideoEncoder() = default;

FFmpegVideoEncoder::~FFmpegVideoEncoder() = default;

bool FFmpegVideoEncoder::Initialize(const Params& params) {
  // Replacing an existing encoder must free it; unique_ptr<Context> alone
  // used to drop it without freeing once ~Context existed.
  ctx_.reset();
  const AVCodec* codec =
      avcodec_find_encoder_by_name(params.codec_name.c_str());
  if (!codec) {
    LOG(ERROR) << "video encoder: \"" << params.codec_name
               << "\" not in this FFmpeg build";
    return false;
  }
  auto c = std::make_unique<Context>();
  c->codec = avcodec_alloc_context3(codec);
  if (!c->codec) {
    return false;
  }
  c->codec->width = params.width;
  c->codec->height = params.height;
  // G6: time_base and framerate come from the caller's fps rational instead
  // of the hardcoded 30. ffmpeg.c sets time_base = av_inv_q(frame_rate),
  // i.e. {fps_den, fps_num}: one tick = one frame interval (30 fps -> 1/30 s
  // per tick). The old {1, fps_den} treated the fps DENOMINATOR as the rate,
  // giving {1,1} -- one tick per SECOND -- for every integer-rate source, so
  // microseconds fed as pts became seconds.
  c->codec->time_base = AVRational{params.fps_den, params.fps_num};
  c->codec->framerate = AVRational{params.fps_num, params.fps_den};
  if (params.bit_rate > 0) {
    c->codec->bit_rate = params.bit_rate;
  }
  if (params.gop > 0) {
    c->codec->gop_size = params.gop;
  }
  if (!params.preset.empty() &&
      av_opt_set(c->codec->priv_data, "preset", params.preset.c_str(), 0) < 0) {
    LOG(WARNING) << "video encoder: unknown preset \"" << params.preset
                 << "\"; using the encoder default";
  }
  if (params.crf >= 0 &&
      av_opt_set_int(c->codec->priv_data, "crf", params.crf, 0) < 0) {
    LOG(WARNING) << "video encoder: crf not supported by this encoder";
  }

  // The conversion target: per-codec negotiation.
  //  * GIF takes PAL8 only and swscale cannot output pal8 -- handled by the
  //    median-cut quantizer in Encode() (the converter targets RGBA).
  //  * Encoders whose native input is full-range J variants (mjpeg) get
  //    YUVJ420P; every codec that accepts the standard 4:2:0 (libx264) gets
  //    YUV420P -- the existing behavior, byte-for-byte.
  //  * Anything else takes the first format its descriptor advertises that
  //    swscale can produce (prores_ks lists yuv422p10le first -- the ProRes
  //    delivery convention).
  const bool is_gif = strcmp(codec->name, "gif") == 0;
  c->is_gif = is_gif;
  const bool full_range = strcmp(codec->name, "mjpeg") == 0;
  bool accepts_420 = codec->pix_fmts == nullptr;
  for (const AVPixelFormat* f = codec->pix_fmts;
       f && *f != AV_PIX_FMT_NONE; ++f) {
    if (*f == AV_PIX_FMT_YUV420P || *f == AV_PIX_FMT_YUVJ420P) {
      accepts_420 = true;
      break;
    }
  }
  // HDR BT.2020/PQ（hevc_videotoolbox + P010LE）：通过 options["hdr"]="1"
  // 启用。输入仍是 YUV420P 8-bit（解码侧输出），编码器上采样到 10-bit
  // P010LE 并标定 BT.2020/PQ 色彩——容器标定能力，不改变像素内容。
  const bool is_hdr = strcmp(codec->name, "hevc_videotoolbox") == 0 &&
                      params.options.count("hdr") > 0 &&
                      params.options.at("hdr") == "1";
  c->is_hdr = is_hdr;
  if (is_gif) {
    c->codec->pix_fmt = AV_PIX_FMT_PAL8;
    c->dst_fmt = AV_PIX_FMT_RGBA;
  } else if (is_hdr) {
    c->codec->pix_fmt = AV_PIX_FMT_P010LE;
    c->dst_fmt = AV_PIX_FMT_P010LE;
    c->codec->color_primaries = AVCOL_PRI_BT2020;
    c->codec->color_trc = AVCOL_TRC_SMPTE2084;
    c->codec->colorspace = AVCOL_SPC_BT2020_NCL;
    c->codec->color_range = AVCOL_RANGE_MPEG;
  } else if (accepts_420) {
    c->dst_fmt = full_range ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_YUV420P;
    c->codec->pix_fmt = c->dst_fmt;
    c->codec->color_range = full_range ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
  } else if (codec->pix_fmts != nullptr) {
    c->dst_fmt = codec->pix_fmts[0];
    c->codec->pix_fmt = c->dst_fmt;
  } else {
    c->dst_fmt = AV_PIX_FMT_YUV420P;
    c->codec->pix_fmt = c->dst_fmt;
  }

  // OWNERSHIP: avcodec_open2 takes AVDictionary** and, on BOTH the success
  // and the failure path, frees the dictionary it was handed and writes back
  // a different one holding the options it did not recognise. So the pointer
  // must be released to it -- keeping it in a DictPtr and freeing it again
  // afterwards is a double free (measured: SIGABRT in the encoder unit test,
  // with the test's own output looking fine up to that point), and freeing
  // nothing at all leaks the unrecognised-options dictionary it allocates.
  // What is freed below is the object avcodec_open2 left in |options_raw|,
  // which is not the one allocated above.
  ff::DictPtr options = ff::ToAvDict(params.options);
  AVDictionary* options_raw = options.release();
  const int open_result = avcodec_open2(c->codec, codec, &options_raw);
  av_dict_free(&options_raw);
  if (open_result < 0) {
    LOG(ERROR) << "video encoder: avcodec_open2 failed for \""
               << params.codec_name << "\"";
    avcodec_free_context(&c->codec);
    return false;
  }

  // The frames coming in are xmedia-kit's decode output: I420 (limited range).
  // Normalize to the encoder's input format through the shared converter.
  c->in_fmt = ff::ToAvPixelFormat(VideoFormat::kI420);
  if (!c->converter.Configure(params.width, params.height, c->in_fmt,
                              params.width, params.height, c->dst_fmt)) {
    LOG(ERROR) << "video encoder: conversion setup failed";
    avcodec_free_context(&c->codec);
    return false;
  }
  if (c->is_gif) {
    c->rgba = ff::MakeFrame();
    c->rgba->width = params.width;
    c->rgba->height = params.height;
    c->rgba->format = c->dst_fmt;
    if (av_frame_get_buffer(c->rgba.get(), 32) != 0) {
      LOG(ERROR) << "video encoder: rgba buffer alloc failed";
      avcodec_free_context(&c->codec);
      return false;
    }
    c->pal = ff::MakeFrame();
    c->pal->width = params.width;
    c->pal->height = params.height;
    c->pal->format = AV_PIX_FMT_PAL8;
    if (av_frame_get_buffer(c->pal.get(), 32) != 0) {
      LOG(ERROR) << "video encoder: pal8 buffer alloc failed";
      avcodec_free_context(&c->codec);
      return false;
    }
    ctx_ = std::move(c);
    return true;
  }
  c->converted = ff::MakeFrame();
  c->converted->width = params.width;
  c->converted->height = params.height;
  c->converted->format = c->dst_fmt;
  if (av_frame_get_buffer(c->converted.get(), 32) != 0) {
    LOG(ERROR) << "video encoder: buffer alloc failed";
    avcodec_free_context(&c->codec);
    return false;
  }
  ctx_ = std::move(c);
  return true;
}

const AVCodecParameters* FFmpegVideoEncoder::codec_parameters() const {
  // Filled from the codec context by avcodec_parameters_from_context on
  // first call; EncodeMuxer copies it into its stream. Owned by the encoder.
  if (!ctx_) {
    return nullptr;
  }
  if (!ctx_->par) {
    ctx_->par = avcodec_parameters_alloc();
    if (avcodec_parameters_from_context(ctx_->par, ctx_->codec) < 0) {
      avcodec_parameters_free(&ctx_->par);
    }
  }
  return ctx_->par;
}

AVRational FFmpegVideoEncoder::time_base() const {
  if (!ctx_) {
    return AVRational{1, 30};
  }
  return ctx_->codec->time_base;
}

int64_t FFmpegVideoEncoder::dedup_skipped_frames() const {
  return ctx_ ? ctx_->dedup_skipped : 0;
}

bool FFmpegVideoEncoder::Encode(base::scoped_refptr<VideoFrame> in,
                                std::vector<EncodedPacket>* out) {
  if (!ctx_ || !out) {
    return false;
  }
  // T9a dedup: hash the input frame's plane 0 via the flat HashBytes64 over
  // its exact allocated span — no stride/row arithmetic, so it cannot
  // over-read (the earlier HashImageRows SIGSEGV came from stride math on a
  // mismatched width contract). A match with the previous frame skips the
  // conversion pipeline and re-sends the cached output frame at the new pts.
  // The encoder still encodes it — the saving is the conversion, not the
  // encode itself. For RGBA sources plane 0 is the whole buffer; for planar
  // sources it is the Y plane — both are stable per-frame content keys.
  const auto input_span = in->visible_data(VideoFrame::Plane::kYPlane);
  // Seed is a fixed constant: identical content at a different pts must
  // still hash equal (mixing the timestamp in would disable dedup entirely).
  const uint64_t input_hash =
      input_span.empty()
          ? 0
          : dedup::HashBytes64(input_span.data(), input_span.size(), 0x9E37);
  const bool is_duplicate =
      ctx_->last_input_valid && input_hash == ctx_->last_input_hash;
  ctx_->last_input_hash = input_hash;
  ctx_->last_input_valid = true;

  if (is_duplicate) {
    // Re-send the cached frame with the new pts (skip conversion entirely).
    // The cached frame (ctx_->converted or ctx_->pal) still holds the
    // previous conversion result — the pixel data is identical, only the
    // timestamp changed.
    ++ctx_->dedup_skipped;
    ff::FramePtr& cached = ctx_->is_gif ? ctx_->pal : ctx_->converted;
    if (cached) {
      cached->pts =
          ff::FromTimeDelta(in->timestamp(), ctx_->codec->time_base);
      const int send = avcodec_send_frame(ctx_->codec, cached.get());
      if (send < 0) {
        LOG(ERROR) << "video encoder: dedup send_frame failed";
        return false;
      }
      ctx_->DrainPackets(out);
      return true;
    }
    // Cache miss (first frame was a dup?) — fall through to normal path.
  }

  // View over the source planes, then convert into the reused target frame.
  // ff::MakeFrame CHECKs internally; there is no failure mode to report.
  ff::FramePtr src = ff::MakeFrame();
  src->width = ctx_->codec->width;
  src->height = ctx_->codec->height;
  src->format = ctx_->in_fmt;
  for (int p = 0; p < VideoFrame::kMaxPlanes; ++p) {
    const auto plane = static_cast<VideoFrame::Plane>(p);
    src->data[p] = const_cast<uint8_t*>(in->visible_data(plane).data());
    src->linesize[p] = in->stride(plane);
  }
  if (ctx_->is_gif) {
    // GIF leg: YUV420P -> RGBA -> median-cut PAL8. The palette is per frame
    // (GIF's own model); the encoder stores it in the frame's palette plane.
    if (!ctx_->converter.Convert(*src, ctx_->rgba->data,
                                 ctx_->rgba->linesize)) {
      LOG(ERROR) << "video encoder: rgba conversion failed";
      return false;
    }
    ctx_->palette.Build(ctx_->rgba->data[0], ctx_->codec->width,
                        ctx_->codec->height, ctx_->rgba->linesize[0]);
    memcpy(ctx_->pal->data[1], ctx_->palette.palette_bgra(),
           AVPALETTE_SIZE);
    ctx_->pal->pts =
        ff::FromTimeDelta(in->timestamp(), ctx_->codec->time_base);
    const int send = avcodec_send_frame(ctx_->codec, ctx_->pal.get());
    if (send < 0) {
      LOG(ERROR) << "video encoder: gif send_frame failed";
      return false;
    }
    ctx_->DrainPackets(out);
    return true;
  }
  if (!ctx_->converter.Convert(*src, ctx_->converted->data,
                               ctx_->converted->linesize)) {
    LOG(ERROR) << "video encoder: conversion failed";
    return false;
  }
  // Range/matrix flow through from the frame so colours survive the trip.
  if (ctx_->is_hdr) {
    ctx_->converted->colorspace = AVCOL_SPC_BT2020_NCL;
    ctx_->converted->color_primaries = AVCOL_PRI_BT2020;
    ctx_->converted->color_trc = AVCOL_TRC_SMPTE2084;
    ctx_->converted->color_range = AVCOL_RANGE_MPEG;
  } else {
    switch (in->color_space().matrix) {
    case ColorMatrix::kBT709:
      ctx_->converted->colorspace = AVCOL_SPC_BT709;
      break;
    case ColorMatrix::kBT2020Ncl:
      ctx_->converted->colorspace = AVCOL_SPC_BT2020_NCL;
      break;
    case ColorMatrix::kSMPTE170M:
      ctx_->converted->colorspace = AVCOL_SPC_SMPTE170M;
      break;
    default:
      ctx_->converted->colorspace = AVCOL_SPC_BT470BG;
      break;
    }
    ctx_->converted->color_range = in->color_space().range == ColorRange::kFull
                                       ? AVCOL_RANGE_JPEG
                                       : AVCOL_RANGE_MPEG;
  }
  // The frame's timestamp is microseconds (xmedia-kit VideoFrame); the
  // encoder's
  // time_base ticks are frame intervals. Rescale instead of feeding raw
  // microseconds: under the old {1, fps_den} time_base a 33333 us frame
  // interval became 33333 SECONDS of timeline.
  ctx_->converted->pts =
      ff::FromTimeDelta(in->timestamp(), ctx_->codec->time_base);

  const int send = avcodec_send_frame(ctx_->codec, ctx_->converted.get());
  if (send < 0) {
    LOG(ERROR) << "video encoder: send_frame failed";
    return false;
  }
  ctx_->DrainPackets(out);
  return true;
}

bool FFmpegVideoEncoder::Flush(std::vector<EncodedPacket>* out) {
  if (!ctx_ || !out) {
    return false;
  }
  if (avcodec_send_frame(ctx_->codec, nullptr) < 0) {
    return false;
  }
  ctx_->DrainPackets(out);
  return true;
}

}  // namespace xmedia::media

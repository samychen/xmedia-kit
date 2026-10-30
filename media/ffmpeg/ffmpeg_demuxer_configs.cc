// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/ffmpeg_demuxer_configs.h"

extern "C" {
#include <libavutil/display.h>
}

#include <cmath>
#include <string>

#include "media/base/decoder_config.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"
#include "media/ffmpeg/ffmpeg_util.h"
#include "media/ffmpeg/tone_map.h"

namespace xmedia::media {

namespace ff = ::xmedia::media::ffmpeg;

namespace {

// Dolby Vision / HDR10+ carry dynamic metadata nothing here can apply, and
// they are worth flagging separately from the transfer function: a Dolby
// Vision stream very often ALSO declares PQ, so "has HDR transfer" would be
// true for it and the host would lose the detail that a static fallback is
// not merely dim but structurally wrong (the DV base layer is not the picture).
bool HasDynamicHdrMetadata(const AVStream* stream) {
  size_t size = 0;
  if (xmedia_stream_side_data(stream, AV_PKT_DATA_DOVI_CONF, &size) !=
          nullptr &&
      size > 0) {
    return true;
  }
  size = 0;
  return xmedia_stream_side_data(stream, AV_PKT_DATA_DYNAMIC_HDR10_PLUS,
                                 &size) != nullptr &&
         size > 0;
}

}  // namespace

VideoDecoderConfig MakeVideoDecoderConfig(const AVStream* stream) {
  VideoDecoderConfig config;
  const AVCodecParameters* par = stream->codecpar;
  const AVCodec* codec = avcodec_find_decoder(par->codec_id);
  config.codec_name = codec ? codec->name : "unknown";
  config.codec = VideoCodecFromName(config.codec_name);
  config.coded_size = Size{par->width, par->height};
  if (par->sample_aspect_ratio.num > 0 && par->sample_aspect_ratio.den > 0) {
    config.sar =
        Rational{par->sample_aspect_ratio.num, par->sample_aspect_ratio.den};
    // natural_size is the SAR-applied display size. Rounding matches FFmpeg's
    // av_reduce so a 720x576 anamorphic stream reports 1024x576.
    const int64_t w = static_cast<int64_t>(par->width) * config.sar.num;
    const int64_t d = config.sar.den;
    config.natural_size = Size{static_cast<int>((w + d / 2) / d), par->height};
  } else {
    config.natural_size = config.coded_size;
  }
  config.frame_rate =
      Rational{stream->avg_frame_rate.num, stream->avg_frame_rate.den};
  // The decoder cannot interpret AVFrame::pts without this (see
  // VideoDecoderConfig::time_base).
  config.time_base = Rational{stream->time_base.num, stream->time_base.den};
  config.avg_frame_rate = config.frame_rate;
  config.bit_rate = par->bit_rate > 0 ? par->bit_rate : 0;
  config.profile = par->profile >= 0 ? std::to_string(par->profile) : "";
  config.level = par->level >= 0 ? std::to_string(par->level) : "";
  // HDR: this field had NO writer in the tree until now -- it existed, it was
  // read by the colour-space fallback table, and it was false for every file,
  // so the HDR half of GuessColorSpaceFallback() was dead code that looked
  // alive. A field with only readers is the same defect
  // class as an option with only a registry entry.
  config.has_hdr_metadata =
      ff::IsHdrTransfer(static_cast<int>(par->color_trc)) ||
      HasDynamicHdrMetadata(stream);
  if (par->extradata && par->extradata_size > 0) {
    config.extra_data.assign(par->extradata,
                             par->extradata +
                                 static_cast<size_t>(par->extradata_size));
  }
  // Rotation lives in a display-matrix side-data entry, not in the codec
  // parameters. legacy reads it in three separate places.
  size_t matrix_size = 0;
  const uint8_t* matrix =
      xmedia_stream_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX, &matrix_size);
  if (matrix && matrix_size >= 9 * sizeof(int32_t)) {
    const double rotation =
        av_display_rotation_get(reinterpret_cast<const int32_t*>(matrix));
    if (!std::isnan(rotation)) {
      int degrees = static_cast<int>(-rotation) % 360;
      if (degrees < 0) {
        degrees += 360;
      }
      config.rotation = degrees;
    }
  }
  return config;
}

AudioDecoderConfig MakeAudioDecoderConfig(const AVStream* stream) {
  AudioDecoderConfig config;
  const AVCodecParameters* par = stream->codecpar;
  const AVCodec* codec = avcodec_find_decoder(par->codec_id);
  config.codec_name = codec ? codec->name : "unknown";
  config.codec = AudioCodecFromName(config.codec_name);
  config.sample_rate = par->sample_rate;
  config.channels = ff::ChannelCount(par);
  switch (config.channels) {
  case 1:
    config.channel_layout = ChannelLayout::kMono;
    break;
  case 2:
    config.channel_layout = ChannelLayout::kStereo;
    break;
  case 6:
    config.channel_layout = ChannelLayout::k5_1;
    break;
  case 8:
    config.channel_layout = ChannelLayout::k7_1;
    break;
  default:
    config.channel_layout = ff::ChannelLayoutMask(par)
                                ? ChannelLayout::kDiscrete
                                : ChannelLayout::kNone;
    break;
  }
  // A container that declares no sample format lands on kUnknown, which is the
  // documented "source did not say" value -- not a guess at a real one.
  // AVCodecParameters::format is an int that carries either a pixel or a sample
  // format depending on codec_type, so it needs the cast the video decoder
  // applies to AVFrame::format.
  config.sample_format = ff::FromAvSampleFormat(
      static_cast<AVSampleFormat>(par->format), SampleFormat::kUnknown);
  config.bit_rate = par->bit_rate > 0 ? par->bit_rate : 0;
  config.codec_delay_frames = par->initial_padding;
  config.seek_preroll_frames = par->seek_preroll;
  if (par->extradata && par->extradata_size > 0) {
    config.extra_data.assign(par->extradata,
                             par->extradata +
                                 static_cast<size_t>(par->extradata_size));
  }
  return config;
}

TextDecoderConfig MakeTextDecoderConfig(const AVStream* stream) {
  TextDecoderConfig config;
  const AVCodecParameters* par = stream->codecpar;
  const AVCodec* codec = avcodec_find_decoder(par->codec_id);
  config.codec_name = codec ? codec->name : "unknown";
  if (par->extradata && par->extradata_size > 0) {
    config.extra_data.assign(par->extradata,
                             par->extradata +
                                 static_cast<size_t>(par->extradata_size));
  }
  return config;
}

}  // namespace xmedia::media

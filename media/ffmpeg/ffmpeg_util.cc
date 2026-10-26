// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/ffmpeg_util.h"

#include "media/ffmpeg/av_includes.h"

namespace avbase::media::ffmpeg {

AVPixelFormat ToAvPixelFormat(media::VideoFormat format) {
  switch (format) {
  case media::VideoFormat::kI420:
  case media::VideoFormat::kYV12:
    return AV_PIX_FMT_YUV420P;  // Plane order differs.
  case media::VideoFormat::kNV12:
    return AV_PIX_FMT_NV12;
  case media::VideoFormat::kNV21:
    return AV_PIX_FMT_NV21;
  case media::VideoFormat::kARGB:
    return AV_PIX_FMT_0RGB32;
  case media::VideoFormat::kRGB24:
    return AV_PIX_FMT_RGB24;
  case media::VideoFormat::kRGB565:
    return AV_PIX_FMT_RGB565LE;
  case media::VideoFormat::kP010:
    return AV_PIX_FMT_P010LE;
  case media::VideoFormat::kYUY2:
  case media::VideoFormat::kYUV420P10:
  case media::VideoFormat::kUnknown:
    break;
  }
  return AV_PIX_FMT_NONE;
}

media::VideoFormat FromAvPixelFormat(AVPixelFormat format,
                                     media::VideoFormat fallback) {
  switch (format) {
  case AV_PIX_FMT_YUV420P:
    return media::VideoFormat::kI420;
  case AV_PIX_FMT_NV12:
    return media::VideoFormat::kNV12;
  case AV_PIX_FMT_NV21:
    return media::VideoFormat::kNV21;
  case AV_PIX_FMT_0RGB32:
  case AV_PIX_FMT_ARGB:
    return media::VideoFormat::kARGB;
  case AV_PIX_FMT_RGB24:
    return media::VideoFormat::kRGB24;
  case AV_PIX_FMT_RGB565LE:
    return media::VideoFormat::kRGB565;
  case AV_PIX_FMT_P010LE:
    return media::VideoFormat::kP010;
  case AV_PIX_FMT_YUV422P:
  case AV_PIX_FMT_YUV444P:
    return media::VideoFormat::kI420;  // Needs conversion.
  default:
    return fallback;
  }
}

AVSampleFormat ToAvSampleFormat(media::SampleFormat format) {
  switch (format) {
  case media::SampleFormat::kU8:
    return AV_SAMPLE_FMT_U8;
  case media::SampleFormat::kS16:
    return AV_SAMPLE_FMT_S16;
  case media::SampleFormat::kS32:
    return AV_SAMPLE_FMT_S32;
  case media::SampleFormat::kF32:
    return AV_SAMPLE_FMT_FLT;
  case media::SampleFormat::kS16P:
    return AV_SAMPLE_FMT_S16P;
  case media::SampleFormat::kS32P:
    return AV_SAMPLE_FMT_S32P;
  case media::SampleFormat::kF32P:
    return AV_SAMPLE_FMT_FLTP;
  default:
    return AV_SAMPLE_FMT_NONE;
  }
}

media::SampleFormat FromAvSampleFormat(AVSampleFormat format,
                                       media::SampleFormat fallback) {
  switch (format) {
  case AV_SAMPLE_FMT_U8:
    return media::SampleFormat::kU8;
  case AV_SAMPLE_FMT_S16:
    return media::SampleFormat::kS16;
  case AV_SAMPLE_FMT_S32:
    return media::SampleFormat::kS32;
  case AV_SAMPLE_FMT_FLT:
    return media::SampleFormat::kF32;
  case AV_SAMPLE_FMT_S16P:
    return media::SampleFormat::kS16P;
  case AV_SAMPLE_FMT_S32P:
    return media::SampleFormat::kS32P;
  case AV_SAMPLE_FMT_FLTP:
    return media::SampleFormat::kF32P;
  default:
    return fallback;
  }
}

AVFormatContext* AsFormatContext(void* raw) {
  return static_cast<AVFormatContext*>(raw);
}

}  // namespace avbase::media::ffmpeg

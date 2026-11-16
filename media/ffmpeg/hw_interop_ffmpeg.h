// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FFMPEG_HW_INTEROP_FFMPEG_H_
#define XMEDIA_MEDIA_FFMPEG_HW_INTEROP_FFMPEG_H_

// 硬件互挪（NVDEC→CUDA-GL 等）测试所需的 libav 头转发：C4 隔离规则
// 只允许 media/ffmpeg/ 直接包含 libav*——gl_cuda 测试经此转发头取
// hwcontext_cuda 类型。本构建的这些头无完整 C++ 守卫，extern "C"
// 手工包（同 swscale/hash 处理）。

#include "media/ffmpeg/av_includes.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_cuda.h>
}

#endif  // XMEDIA_MEDIA_FFMPEG_HW_INTEROP_FFMPEG_H_

// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_LOTTIE_LOTTIE_CLOCK_H_
#define XMEDIA_MEDIA_LOTTIE_LOTTIE_CLOCK_H_

// Lottie 时钟映射（MC lottie_clock 对齐，纯逻辑、无 rlottie 依赖）：
//   合成毫秒 t_ms（bake seek/VT 时钟；实时 = 墙钟）
//     → lottie 自身时间轴帧号 = floor(t_ms/1000 * src_fps * speed)，
//       loop 时对 total_frame 取模回绕，否则钳到 [0, total-1]。
// 同一 t_ms 恒得同一帧号 → 同一帧号恒得同一像素（rlottie renderSync 是
// 纯函数式 CPU 光栅化）——与关键帧求值共用同一确定性时钟基准。
//
// JSON 参数解析未移植（xmedia 的消费方直接接受结构体；场景加载器出现时
// 再补 ParseLottieSourceParams）。

#include <cstdint>
#include <string>

#include "media/media_export.h"

namespace xmedia {
namespace lottie {

// scene lottie source 参数：path 由调用方解析；speed > 0（1 = 原速）；
// loop = 到尾回绕（false = 钳在最后一帧）。
struct XMEDIA_MEDIA_EXPORT LottieSourceParams {
  std::string path;
  double speed = 1.0;
  bool loop = true;
};

// 合成毫秒 → lottie 帧号。speed<=0 视为 1；src_fps<=0 或 total_frames<=0
// 返回 0（坏文件在加载期已被拒绝，此处兜底停在第 0 帧）。
int XMEDIA_MEDIA_EXPORT ComputeLottieFrameIndex(int64_t anim_ms, double speed,
                                                double src_fps,
                                                int total_frames, bool loop);

// lottie 渲染面尺寸：保持宽高比把 (w,h) 缩到 max_dim 内。w/h<=0 或缩放
// 后 <=0 视为非法返回 false；结果至少 1x1。
bool XMEDIA_MEDIA_EXPORT ComputeLottieSurfaceSize(int w, int h, int max_dim,
                                                  int* out_w, int* out_h);

}  // namespace lottie
}  // namespace xmedia

#endif  // XMEDIA_MEDIA_LOTTIE_LOTTIE_CLOCK_H_

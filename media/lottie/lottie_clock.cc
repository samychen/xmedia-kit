// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Lottie 时钟映射实现（MC lottie_clock.cpp 对齐移植）。

#include "media/lottie/lottie_clock.h"

#include <algorithm>
#include <cmath>

namespace xmedia {
namespace lottie {

int ComputeLottieFrameIndex(int64_t anim_ms, double speed, double src_fps,
                            int total_frames, bool loop) {
  if (total_frames <= 0 || src_fps <= 0.0) {
    return 0;  // 坏文件在加载期已被拒绝；此处兜底停在第 0 帧。
  }
  const double speed_eff = speed > 0.0 ? speed : 1.0;
  // double fmod 在同机同二进制下确定；t_ms 为整型输入，speed/fps 为加载
  // 期固定值——同一帧号重复求值必得同一结果。
  const double src_frame =
      static_cast<double>(anim_ms) / 1000.0 * src_fps * speed_eff;
  double idx = std::floor(src_frame);
  if (idx < 0.0) {
    idx = 0.0;
  }
  if (loop) {
    idx = std::fmod(idx, static_cast<double>(total_frames));
  } else if (idx > static_cast<double>(total_frames - 1)) {
    idx = static_cast<double>(total_frames - 1);
  }
  int out = static_cast<int>(idx);
  if (out < 0) {
    out = 0;
  }
  if (out > total_frames - 1) {
    out = total_frames - 1;
  }
  return out;
}

bool ComputeLottieSurfaceSize(int w, int h, int max_dim, int* out_w,
                              int* out_h) {
  if (out_w == nullptr || out_h == nullptr || w <= 0 || h <= 0 ||
      max_dim <= 0) {
    return false;
  }
  const double scale =
      std::min(1.0, static_cast<double>(max_dim) /
                        static_cast<double>(std::max(w, h)));
  *out_w = std::max(1, static_cast<int>(static_cast<double>(w) * scale));
  *out_h = std::max(1, static_cast<int>(static_cast<double>(h) * scale));
  return true;
}

}  // namespace lottie
}  // namespace xmedia

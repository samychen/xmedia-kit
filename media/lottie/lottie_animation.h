// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_LOTTIE_LOTTIE_ANIMATION_H_
#define XMEDIA_MEDIA_LOTTIE_LOTTIE_ANIMATION_H_

// Lottie 动画模型封装（MC lottie_layer_producer 对齐）：rlottie::Animation
// 只读模型 + 单线程 renderSync（纯函数式 CPU 光栅化——同一帧号恒得同一像
// 素，bake 确定性的前提）。rlottie.h 仅在 .cpp 可见（依赖不外泄）。
//
// 输出：kARGB VideoFrame（B,G,R,A 内存序，直通 GlScene 的 RGB 上传路径）；
// rlottie 的 BGRA premultiplied 输出就地转直通 alpha。
//
// 构建门控：XMEDIA_HAS_LOTTIE=0（tools/build 缺 librlottie.a）时本头文件
// 存在但所有符号编译为空壳。

#include <memory>

#include "base/memory/scoped_refptr.h"
#include "media/base/video_frame.h"
#include "media/lottie/lottie_clock.h"
#include "media/media_export.h"

namespace xmedia {
namespace lottie {

class XMEDIA_MEDIA_EXPORT LottieAnimation {
 public:
  // 打开 |params.path| 的 lottie JSON；渲染面按宽高比钳到 max_dim。
  // 失败返回 null（坏文件不重试——每帧重读毫无意义）。
  static std::unique_ptr<LottieAnimation> Open(const LottieSourceParams& params,
                                               int max_surface_dim);
  ~LottieAnimation();
  LottieAnimation(const LottieAnimation&) = delete;
  LottieAnimation& operator=(const LottieAnimation&) = delete;

  // 渲染 |anim_ms| 对应帧（时钟映射经 ComputeLottieFrameIndex：speed/loop
  // 语义在此生效）。返回 kARGB VideoFrame（表面尺寸，帧内容不变则像素
  // 确定性相同）；失败返回 null。
  base::scoped_refptr<media::VideoFrame> RenderAtMs(int64_t anim_ms);

  int total_frame() const { return total_frame_; }
  double src_fps() const { return src_fps_; }
  int surface_width() const { return surface_w_; }
  int surface_height() const { return surface_h_; }

 private:
  LottieAnimation();
  struct Impl;
  std::unique_ptr<Impl> impl_;
  int total_frame_ = 0;
  double src_fps_ = 0.0;
  int surface_w_ = 0;
  int surface_h_ = 0;
};

}  // namespace lottie
}  // namespace xmedia

#endif  // XMEDIA_MEDIA_LOTTIE_LOTTIE_ANIMATION_H_

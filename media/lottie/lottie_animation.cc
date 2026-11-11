// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// rlottie 封装实现（rlottie.h 仅此 TU 可见）。premultiplied BGRA → 直通
// alpha 的 kARGB（B,G,R,A）：premultiplied 的 B/G/R 都 ≤ A，un-premultiply
// 用 255/a 归一（a=0 → 全 0）。

#include "media/lottie/lottie_animation.h"

#include <algorithm>

#include "base/logging.h"
#include "rlottie/rlottie.h"

namespace xmedia {
namespace lottie {

using media::VideoFrame;

struct LottieAnimation::Impl {
  std::unique_ptr<rlottie::Animation> anim;
  std::vector<uint32_t> render_buf;  // rlottie BGRA（premultiplied）
};

LottieAnimation::LottieAnimation() = default;

LottieAnimation::~LottieAnimation() = default;

std::unique_ptr<LottieAnimation> LottieAnimation::Open(
    const LottieSourceParams& params, int max_surface_dim) {
  auto anim = rlottie::Animation::loadFromFile(params.path);
  if (anim == nullptr) {
    LOG(ERROR) << "lottie: cannot load " << params.path;
    return nullptr;
  }
  size_t w = 0;
  size_t h = 0;
  anim->size(w, h);
  int sw = 0;
  int sh = 0;
  if (!ComputeLottieSurfaceSize(static_cast<int>(w), static_cast<int>(h),
                                max_surface_dim, &sw, &sh)) {
    LOG(ERROR) << "lottie: bad surface size " << w << "x" << h;
    return nullptr;
  }
  auto out = std::unique_ptr<LottieAnimation>(new LottieAnimation());
  out->impl_ = std::make_unique<Impl>();
  out->impl_->anim = std::move(anim);
  out->impl_->render_buf.resize(static_cast<size_t>(sw) * sh);
  out->total_frame_ = static_cast<int>(out->impl_->anim->totalFrame());
  out->src_fps_ = out->impl_->anim->frameRate();
  out->surface_w_ = sw;
  out->surface_h_ = sh;
  if (out->total_frame_ <= 0 || out->src_fps_ <= 0.0) {
    LOG(ERROR) << "lottie: empty animation (frames=" << out->total_frame_
               << " fps=" << out->src_fps_ << ")";
    return nullptr;
  }
  return out;
}

base::scoped_refptr<VideoFrame> LottieAnimation::RenderAtMs(int64_t anim_ms) {
  if (impl_ == nullptr) {
    return nullptr;
  }
  const int frame = ComputeLottieFrameIndex(anim_ms, /*speed=*/1.0, src_fps_,
                                            total_frame_, /*loop=*/true);
  const auto surface = rlottie::Surface(reinterpret_cast<uint32_t*>(
                                            impl_->render_buf.data()),
                                        static_cast<size_t>(surface_w_),
                                        static_cast<size_t>(surface_h_),
                                        static_cast<size_t>(surface_w_) * 4);
  impl_->anim->renderSync(static_cast<size_t>(frame), surface);

  // BGRA premultiplied → kARGB 直通 alpha（B,G,R,A 内存序不变，只是
  // un-premultiply：c' = c * 255 / a）。
  auto out = VideoFrame::CreateBlackFrame(
      media::VideoFormat::kARGB, media::Size{surface_w_, surface_h_},
      media::Size{surface_w_, surface_h_}, media::Rational{1, 1},
      xmedia::base::Milliseconds(anim_ms),
      xmedia::base::Milliseconds(1000 / std::max(1, total_frame_)), 0);
  const auto dst = out->mutable_data(VideoFrame::kYPlane);
  const size_t n = static_cast<size_t>(surface_w_) * surface_h_;
  for (size_t i = 0; i < n; ++i) {
    const uint32_t px = impl_->render_buf[i];
    const uint8_t b = static_cast<uint8_t>(px & 0xff);
    const uint8_t g = static_cast<uint8_t>((px >> 8) & 0xff);
    const uint8_t r = static_cast<uint8_t>((px >> 16) & 0xff);
    const uint8_t a = static_cast<uint8_t>((px >> 24) & 0xff);
    uint8_t* d = dst.data() + i * 4;
    if (a == 0) {
      d[0] = d[1] = d[2] = d[3] = 0;
    } else {
      d[0] = static_cast<uint8_t>(std::min(255, b * 255 / a));
      d[1] = static_cast<uint8_t>(std::min(255, g * 255 / a));
      d[2] = static_cast<uint8_t>(std::min(255, r * 255 / a));
      d[3] = a;
    }
  }
  return out;
}

}  // namespace lottie
}  // namespace xmedia

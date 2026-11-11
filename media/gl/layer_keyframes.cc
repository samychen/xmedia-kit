// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// 逐属性关键帧求值 + CSS 缓动求解（MC layer_keyframes.cpp 对齐移植；数学
// 同 WebKit UnitBezier）。纯逻辑、无 GL / 无 jsoncpp 依赖。

#include "media/gl/layer_keyframes.h"

#include <algorithm>
#include <cmath>

namespace xmedia {
namespace gl {

namespace {

// CSS 具名缓动对应的固定控制点（linear 直接返回 u，不进贝塞尔求解）。
constexpr float kEaseInBezier[4] = {0.42f, 0.f, 1.f, 1.f};
constexpr float kEaseOutBezier[4] = {0.f, 0.f, 0.58f, 1.f};
constexpr float kEaseInOutBezier[4] = {0.42f, 0.f, 0.58f, 1.f};

// cubic-bezier 求值（同 WebKit UnitBezier / CSS 缓动）：
//   x(t) = ((ax*t + bx)*t + cx)*t，由控制点 (x1,y1) (x2,y2) 展开。
// 先对 x 反解参数 t（Newton-Raphson，失败兜底二分），再代入 y(t)。
struct BezierCoeffs {
  float ax = 0.f;
  float bx = 0.f;
  float cx = 0.f;
  float ay = 0.f;
  float by = 0.f;
  float cy = 0.f;
};

BezierCoeffs MakeBezierCoeffs(float x1, float y1, float x2, float y2) {
  BezierCoeffs c;
  c.cx = 3.f * x1;
  c.bx = 3.f * (x2 - x1) - c.cx;
  c.ax = 1.f - c.cx - c.bx;
  c.cy = 3.f * y1;
  c.by = 3.f * (y2 - y1) - c.cy;
  c.ay = 1.f - c.cy - c.by;
  return c;
}

float BezierSampleY(const BezierCoeffs& c, float t) {
  return ((c.ay * t + c.by) * t + c.cy) * t;
}

float BezierSampleX(const BezierCoeffs& c, float t) {
  return ((c.ax * t + c.bx) * t + c.cx) * t;
}

float BezierSampleDX(const BezierCoeffs& c, float t) {
  return (3.f * c.ax * t + 2.f * c.bx) * t + c.cx;
}

float SolveBezierX(float x1, float x2, float x) {
  constexpr int kNewtonIters = 8;
  constexpr int kBisectIters = 16;
  constexpr float kEpsilon = 1e-6f;
  // 只解 x 轴：y 控制点置 0（y 由 SolveCubicBezier 用完整系数另算）。
  const BezierCoeffs cx = MakeBezierCoeffs(x1, 0.f, x2, 0.f);
  x = std::clamp(x, 0.f, 1.f);

  // Newton-Raphson：导数过小/未收敛 → 二分兜底（x(t) 单调，必收敛）。
  float t = x;
  for (int i = 0; i < kNewtonIters; ++i) {
    const float err = BezierSampleX(cx, t) - x;
    if (std::fabs(err) < kEpsilon) {
      return t;
    }
    const float d = BezierSampleDX(cx, t);
    if (std::fabs(d) < kEpsilon) {
      break;
    }
    t -= err / d;
  }
  float lo = 0.f;
  float hi = 1.f;
  t = std::clamp(t, 0.f, 1.f);
  for (int i = 0; i < kBisectIters; ++i) {
    const float err = BezierSampleX(cx, t) - x;
    if (std::fabs(err) < kEpsilon) {
      break;
    }
    if (err < 0.f) {
      lo = t;
    } else {
      hi = t;
    }
    t = 0.5f * (lo + hi);
  }
  return t;
}

float SolveCubicBezier(float u, const float bez[4]) {
  const BezierCoeffs c = MakeBezierCoeffs(bez[0], bez[1], bez[2], bez[3]);
  return BezierSampleY(c, SolveBezierX(bez[0], bez[2], u));
}

}  // namespace

float SolveEase(EaseType ease, float u, const float bezier[4]) {
  if (u <= 0.f) {
    return 0.f;
  }
  if (u >= 1.f) {
    return 1.f;
  }
  switch (ease) {
    case EaseType::Linear:
      return u;
    case EaseType::EaseIn:
      return SolveCubicBezier(u, kEaseInBezier);
    case EaseType::EaseOut:
      return SolveCubicBezier(u, kEaseOutBezier);
    case EaseType::EaseInOut:
      return SolveCubicBezier(u, kEaseInOutBezier);
    case EaseType::CubicBezier:
      return bezier ? SolveCubicBezier(u, bezier) : u;
  }
  return u;
}

float EvaluatePropertyKeyframes(const std::vector<PropertyKeyframe>& keys,
                                int64_t t_ms, float default_value) {
  if (keys.empty()) {
    return default_value;
  }
  if (t_ms <= keys.front().t_ms) {
    return keys.front().value;
  }
  if (t_ms >= keys.back().t_ms) {
    return keys.back().value;
  }
  for (size_t i = 1; i < keys.size(); ++i) {
    if (t_ms > keys[i].t_ms) {
      continue;
    }
    const PropertyKeyframe& a = keys[i - 1];
    const PropertyKeyframe& b = keys[i];
    const int64_t span = b.t_ms - a.t_ms;
    if (span <= 0) {
      return b.value;
    }
    const float u =
        static_cast<float>(t_ms - a.t_ms) / static_cast<float>(span);
    const float eased = SolveEase(a.ease, u, a.bezier);
    return a.value + (b.value - a.value) * eased;
  }
  return keys.back().value;
}

void EvaluateLayerAnimation(const LayerPropertyAnimation& anim, int64_t t_ms,
                            AnimatedLayerValues* out) {
  if (out == nullptr) {
    return;
  }
  *out = AnimatedLayerValues{};
  if (!anim.x.empty()) {
    out->has_x = true;
    out->x = EvaluatePropertyKeyframes(anim.x, t_ms, 0.f);
  }
  if (!anim.y.empty()) {
    out->has_y = true;
    out->y = EvaluatePropertyKeyframes(anim.y, t_ms, 0.f);
  }
  if (!anim.scale.empty()) {
    out->has_scale = true;
    out->scale = EvaluatePropertyKeyframes(anim.scale, t_ms, 1.f);
  }
  if (!anim.opacity.empty()) {
    out->has_opacity = true;
    out->opacity = EvaluatePropertyKeyframes(anim.opacity, t_ms, 1.f);
  }
}

}  // namespace gl
}  // namespace xmedia

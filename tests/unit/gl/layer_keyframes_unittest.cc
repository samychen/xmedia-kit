// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.
//
// 逐属性关键帧求值 + CSS 缓动求解（MC layer_keyframes 对齐）。钉住的值
// 取自 CSS 规范语义：ease_in_out(0.5) ≈ 0.5（对称曲线）、线性插值端点、
// 区间外钳制。

#include "media/gl/layer_keyframes.h"

#include <vector>

#include "gtest/gtest.h"

namespace xmedia {
namespace gl {
namespace {

std::vector<PropertyKeyframe> Pair(int64_t t0, float v0, int64_t t1,
                                   float v1, EaseType ease = EaseType::Linear) {
  PropertyKeyframe a;
  a.t_ms = t0;
  a.value = v0;
  a.ease = ease;
  PropertyKeyframe b;
  b.t_ms = t1;
  b.value = v1;
  return {a, b};
}

}  // namespace

TEST(SolveEaseTest, LinearIsIdentity) {
  EXPECT_FLOAT_EQ(SolveEase(EaseType::Linear, 0.f, nullptr), 0.f);
  EXPECT_FLOAT_EQ(SolveEase(EaseType::Linear, 0.25f, nullptr), 0.25f);
  EXPECT_FLOAT_EQ(SolveEase(EaseType::Linear, 1.f, nullptr), 1.f);
}

TEST(SolveEaseTest, EndpointsClampRegardlessOfCurve) {
  for (EaseType e :
       {EaseType::Linear, EaseType::EaseIn, EaseType::EaseOut,
        EaseType::EaseInOut}) {
    EXPECT_FLOAT_EQ(SolveEase(e, 0.f, nullptr), 0.f) << "ease " << int(e);
    EXPECT_FLOAT_EQ(SolveEase(e, 1.f, nullptr), 1.f) << "ease " << int(e);
  }
}

TEST(SolveEaseTest, EaseInOutAtMidpointIsNearHalf) {
  // 对称曲线的中点进度 = 0.5（CSS ease-in-out 的标志性性质）。
  EXPECT_NEAR(SolveEase(EaseType::EaseInOut, 0.5f, nullptr), 0.5f, 0.01f);
  // 缓动在两侧把进度压向端点：u=0.25 的进度 < 0.25。
  EXPECT_LT(SolveEase(EaseType::EaseInOut, 0.25f, nullptr), 0.25f);
  EXPECT_GT(SolveEase(EaseType::EaseInOut, 0.75f, nullptr), 0.75f);
}

TEST(EvaluatePropertyKeyframesTest, EmptyReturnsDefault) {
  EXPECT_FLOAT_EQ(EvaluatePropertyKeyframes({}, 100, 0.7f), 0.7f);
}

TEST(EvaluatePropertyKeyframesTest, ClampsOutsideTheRange) {
  auto keys = Pair(100, 0.2f, 200, 0.8f);
  EXPECT_FLOAT_EQ(EvaluatePropertyKeyframes(keys, 0, 0.f), 0.2f);
  EXPECT_FLOAT_EQ(EvaluatePropertyKeyframes(keys, 50, 0.f), 0.2f);
  EXPECT_FLOAT_EQ(EvaluatePropertyKeyframes(keys, 999, 0.f), 0.8f);
}

TEST(EvaluatePropertyKeyframesTest, LinearMidpointIsHalfway) {
  auto keys = Pair(0, 0.f, 100, 1.f);
  EXPECT_FLOAT_EQ(EvaluatePropertyKeyframes(keys, 50, 0.f), 0.5f);
  EXPECT_NEAR(EvaluatePropertyKeyframes(keys, 25, 0.f), 0.25f, 1e-6f);
}

TEST(EvaluatePropertyKeyframesTest, EaseSlowsThenCatchesUp) {
  auto keys = Pair(0, 0.f, 100, 1.f, EaseType::EaseInOut);
  // u=0.25 的缓动进度 < 0.25（前段慢）；中点 ≈ 0.5。
  EXPECT_LT(EvaluatePropertyKeyframes(keys, 25, 0.f), 0.25f);
  EXPECT_NEAR(EvaluatePropertyKeyframes(keys, 50, 0.f), 0.5f, 0.01f);
}

TEST(EvaluateLayerAnimationTest, FillsOnlyPresentProperties) {
  LayerPropertyAnimation anim;
  anim.opacity = Pair(0, 0.f, 100, 1.f);
  AnimatedLayerValues out;
  EvaluateLayerAnimation(anim, 50, &out);
  EXPECT_FALSE(out.has_x);
  EXPECT_FALSE(out.has_scale);
  EXPECT_TRUE(out.has_opacity);
  EXPECT_FLOAT_EQ(out.opacity, 0.5f);
  // 无 opacity 关键帧 → has_opacity=false（静态语义）。
  AnimatedLayerValues none;
  EvaluateLayerAnimation(LayerPropertyAnimation{}, 50, &none);
  EXPECT_FALSE(none.has_opacity);
  EXPECT_FLOAT_EQ(none.opacity, 1.f);
}

}  // namespace gl
}  // namespace xmedia

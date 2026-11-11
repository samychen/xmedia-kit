// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.
//
// Lottie 时钟映射：帧号确定性（同一 t_ms 恒得同一帧号）、loop 回绕、
// 非 loop 钳制、速度倍率、坏参数兜底、表面尺寸钳制。

#include "media/lottie/lottie_clock.h"

#include "gtest/gtest.h"

namespace xmedia {
namespace lottie {
namespace {

// 30fps、60 帧的动画（2s）。
constexpr double kFps = 30.0;
constexpr int kFrames = 60;

}  // namespace

TEST(ComputeLottieFrameIndexTest, ZeroMsIsFrameZero) {
  EXPECT_EQ(ComputeLottieFrameIndex(0, 1.0, kFps, kFrames, true), 0);
}

TEST(ComputeLottieFrameIndexTest, MapsMsToFrameAtSourceRate) {
  // 500ms @ 30fps = 帧 15。
  EXPECT_EQ(ComputeLottieFrameIndex(500, 1.0, kFps, kFrames, true), 15);
  // 1000ms = 帧 30。
  EXPECT_EQ(ComputeLottieFrameIndex(1000, 1.0, kFps, kFrames, true), 30);
}

TEST(ComputeLottieFrameIndexTest, LoopWrapsAtTotal) {
  // 2.1s = 帧 63 → 63 % 60 = 3。
  EXPECT_EQ(ComputeLottieFrameIndex(2100, 1.0, kFps, kFrames, true), 3);
}

TEST(ComputeLottieFrameIndexTest, NoLoopClampsToLast) {
  EXPECT_EQ(ComputeLottieFrameIndex(2100, 1.0, kFps, kFrames, false),
            kFrames - 1);
  EXPECT_EQ(ComputeLottieFrameIndex(99000, 1.0, kFps, kFrames, false),
            kFrames - 1);
}

TEST(ComputeLottieFrameIndexTest, SpeedScalesTheClock) {
  // 2x 速：500ms 走到帧 30。
  EXPECT_EQ(ComputeLottieFrameIndex(500, 2.0, kFps, kFrames, true), 30);
}

TEST(ComputeLottieFrameIndexTest, BadParamsFallBackToFrameZero) {
  EXPECT_EQ(ComputeLottieFrameIndex(500, 1.0, 0.0, kFrames, true), 0);
  EXPECT_EQ(ComputeLottieFrameIndex(500, 1.0, kFps, 0, true), 0);
  EXPECT_EQ(ComputeLottieFrameIndex(500, -3.0, kFps, kFrames, true), 15);
}

TEST(ComputeLottieSurfaceSizeTest, KeepsAspectWithinMaxDim) {
  int w = 0;
  int h = 0;
  ASSERT_TRUE(ComputeLottieSurfaceSize(1920, 1080, 640, &w, &h));
  EXPECT_EQ(w, 640);
  EXPECT_EQ(h, 360);
  ASSERT_TRUE(ComputeLottieSurfaceSize(100, 100, 640, &w, &h));
  EXPECT_EQ(w, 100);
  EXPECT_EQ(h, 100);  // 已在限内：1:1 不放大。
}

TEST(ComputeLottieSurfaceSizeTest, RejectsBadInput) {
  int w = 0;
  int h = 0;
  EXPECT_FALSE(ComputeLottieSurfaceSize(0, 100, 640, &w, &h));
  EXPECT_FALSE(ComputeLottieSurfaceSize(100, 100, 0, &w, &h));
}

}  // namespace lottie
}  // namespace xmedia

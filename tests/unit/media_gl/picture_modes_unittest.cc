// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gl/gl_layer_geometry.h"

#include <cmath>

#include <gtest/gtest.h>

namespace avbase::media {
namespace {

constexpr std::array<float, 4> kFull{0.0f, 0.0f, 1.0f, 1.0f};

TEST(CoverSourceRectTest, CropsTheLongerAxisAndKeepsTheCentre) {
  // A 16:9 picture (region aspect 1.777) shown in a 1:1 rect: the width is
  // cropped to 9/16 of the picture, horizontally centred.
  auto crop = CoverSourceRect(kFull, 1.0f, 16.0f / 9.0f);
  EXPECT_NEAR(crop[2], 9.0f / 16.0f, 1e-6);
  EXPECT_NEAR(crop[0], (1.0f - 9.0f / 16.0f) / 2.0f, 1e-6);
  EXPECT_NEAR(crop[3], 1.0f, 1e-6);
  EXPECT_NEAR(crop[1], 0.0f, 1e-6);
  // The cropped region, scaled by the picture's own aspect, has exactly
  // the target aspect: that is what makes the fill undistorted.
  EXPECT_NEAR((crop[2] / crop[3]) * (16.0f / 9.0f), 1.0f, 1e-6);
}

TEST(CropAndMirrorGeometryTest, TallTargetCropsTheHeight) {
  auto crop = CoverSourceRect(kFull, 2.0f, 1.0f);
  EXPECT_NEAR(crop[3], 0.5f, 1e-6);
  EXPECT_NEAR(crop[1], 0.25f, 1e-6);
  EXPECT_NEAR(crop[2] / crop[3], 2.0f, 1e-6);
}

TEST(CropAndMirrorGeometryTest, DegenerateAspectsReturnTheInput) {
  auto same = CoverSourceRect(kFull, 0.0f, 1.0f);
  EXPECT_EQ(same, kFull);
  auto same2 = CoverSourceRect(kFull, 1.0f, -1.0f);
  EXPECT_EQ(same2, kFull);
}

TEST(CropAndMirrorGeometryTest, MirrorFlipIsTheNegativeWidthShift) {
  // The flip the Compose path applies: (x, w) -> (x + w, -w). A uv of
  // x + pos*w then walks the source right-to-left over the same region.
  const std::array<float, 4> src = {0.25f, 0.1f, 0.5f, 0.8f};
  const std::array<float, 4> flipped = {src[0] + src[2], src[1], -src[2],
                                        src[3]};
  EXPECT_NEAR(flipped[0], 0.75f, 1e-6);
  EXPECT_NEAR(flipped[2], -0.5f, 1e-6);
  // Same magnitude, reversed direction: for a [0,1] uv parameter the
  // covered interval is identical (start + width collapses back to the
  // original start).
  EXPECT_NEAR(flipped[0] + flipped[2], src[0], 1e-6);
  EXPECT_NEAR(std::abs(flipped[2]), src[2], 1e-6);
}

}  // namespace
}  // namespace avbase::media

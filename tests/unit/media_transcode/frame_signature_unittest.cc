// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Frame signature computation and comparison tests. Pins the 32x18 luma
// grid against known-colour frames, verifies RMS comparison semantics, and
// exercises the JSON sidecar round-trip.

#include "media/transcode/frame_signature.h"

#include <cmath>
#include <vector>

#include "gtest/gtest.h"

namespace xmedia::media::dedup {
namespace {

// Solid RGBA frame helper.
std::vector<uint8_t> SolidRgba(int w, int h, uint8_t r, uint8_t g, uint8_t b) {
  std::vector<uint8_t> out(static_cast<size_t>(w) * h * 4);
  for (int i = 0; i < w * h; ++i) {
    out[i * 4 + 0] = r;
    out[i * 4 + 1] = g;
    out[i * 4 + 2] = b;
    out[i * 4 + 3] = 255;
  }
  return out;
}

}  // namespace

TEST(FrameSignatureTest, SolidFrameProducesUniformGrid) {
  auto rgba = SolidRgba(64, 36, 128, 64, 32);
  FrameVerifyRecord rec;
  ASSERT_TRUE(ComputeRgbaLumaSignature(rgba.data(), 0, 64, 36, false, &rec));
  ASSERT_EQ(rec.grid.size(),
            static_cast<size_t>(kFrameVerifyGridW * kFrameVerifyGridH));
  // Rec.601 luma of (128,64,32) ≈ 79. Every grid cell should be close.
  for (uint8_t v : rec.grid) {
    EXPECT_NEAR(v, 79, 2) << "luma grid cell should be ~79 for uniform input";
  }
}

TEST(FrameSignatureTest, YuvPlaneSignatureMatchesRgbaForGrey) {
  // Grey (128,128,128) in RGBA has Rec.601 luma = 128.
  // YUV420P Y plane with Y=128 also has luma 128.
  const int kW = 64, kH = 36;
  auto rgba = SolidRgba(kW, kH, 128, 128, 128);
  std::vector<uint8_t> y_plane(static_cast<size_t>(kW) * kH, 128);

  FrameVerifyRecord rec_rgba, rec_yuv;
  ASSERT_TRUE(ComputeRgbaLumaSignature(rgba.data(), 0, kW, kH, false,
                                       &rec_rgba));
  ASSERT_TRUE(ComputeYuvLumaSignature(y_plane.data(), kW, kW, kH, &rec_yuv));

  auto result = CompareFrameVerifyRecords(rec_rgba, rec_yuv, 2.0);
  EXPECT_TRUE(result.pass) << "RMS=" << result.rms_diff;
}

TEST(FrameSignatureTest, ComparisonDetectsContentDifference) {
  auto frame_a = SolidRgba(32, 18, 255, 0, 0);    // red
  auto frame_b = SolidRgba(32, 18, 0, 0, 255);    // blue
  FrameVerifyRecord rec_a, rec_b;
  ASSERT_TRUE(ComputeRgbaLumaSignature(frame_a.data(), 0, 32, 18, false,
                                       &rec_a));
  ASSERT_TRUE(ComputeRgbaLumaSignature(frame_b.data(), 0, 32, 18, false,
                                       &rec_b));
  auto result = CompareFrameVerifyRecords(rec_a, rec_b, 8.0);
  // Red luma ≈ 76, blue luma ≈ 29 → RMS > 8.
  EXPECT_FALSE(result.pass);
  EXPECT_GT(result.rms_diff, 8.0);
}

TEST(FrameSignatureTest, SameContentPassesWithTolerance) {
  auto frame = SolidRgba(64, 36, 100, 100, 100);
  FrameVerifyRecord rec_a, rec_b;
  ASSERT_TRUE(ComputeRgbaLumaSignature(frame.data(), 0, 64, 36, false,
                                       &rec_a));
  ASSERT_TRUE(ComputeRgbaLumaSignature(frame.data(), 0, 64, 36, false,
                                       &rec_b));
  auto result = CompareFrameVerifyRecords(rec_a, rec_b, 8.0);
  EXPECT_TRUE(result.pass);
  EXPECT_FLOAT_EQ(result.rms_diff, 0.0);
}

TEST(FrameSignatureTest, SerializationRoundTrip) {
  std::vector<FrameVerifyRecord> records;
  for (int i = 0; i < 3; ++i) {
    FrameVerifyRecord rec;
    rec.frame_index = i;
    rec.pts_ms = i * 40;
    rec.grid.assign(static_cast<size_t>(kFrameVerifyGridW * kFrameVerifyGridH),
                    static_cast<uint8_t>(i * 30));
    records.push_back(std::move(rec));
  }
  const std::string json =
      SerializeFrameVerifyRecords(kFrameVerifyGridW, kFrameVerifyGridH, 25.0,
                                  3, records);
  int gw = 0, gh = 0;
  double fps = 0;
  int64_t fc = 0;
  std::vector<FrameVerifyRecord> parsed;
  ASSERT_TRUE(ParseFrameVerifyRecords(json, &gw, &gh, &fps, &fc, &parsed));
  EXPECT_EQ(gw, kFrameVerifyGridW);
  EXPECT_EQ(gh, kFrameVerifyGridH);
  EXPECT_EQ(parsed.size(), 3u);
  for (size_t i = 0; i < parsed.size(); ++i) {
    EXPECT_EQ(parsed[i].frame_index, static_cast<int64_t>(i));
    EXPECT_EQ(parsed[i].grid.size(),
              static_cast<size_t>(kFrameVerifyGridW * kFrameVerifyGridH));
    EXPECT_EQ(parsed[i].grid[0],
              static_cast<uint8_t>(static_cast<int>(i) * 30));
  }
}

TEST(FrameSignatureTest, ParseRejectsMalformedJson) {
  int gw = 0, gh = 0;
  double fps = 0;
  int64_t fc = 0;
  std::vector<FrameVerifyRecord> records;
  EXPECT_FALSE(ParseFrameVerifyRecords("not json", &gw, &gh, &fps, &fc,
                                       &records));
  EXPECT_FALSE(ParseFrameVerifyRecords("{}", &gw, &gh, &fps, &fc, &records));
}

}  // namespace xmedia::media::dedup

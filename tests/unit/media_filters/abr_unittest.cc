// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/abr_algorithm.h"
#include "media/filters/abr_bandwidth_estimator.h"

#include "gtest/gtest.h"

namespace avbase::media {
namespace {

AbrInput MakeInput(std::vector<int64_t> ladder, int current,
                   int64_t bandwidth_bps, base::TimeDelta buffered,
                   base::TimeDelta capacity) {
  AbrInput input;
  input.variant_bandwidths_bps = std::move(ladder);
  input.current_index = current;
  input.current_variant_bandwidth_bps = input.variant_bandwidths_bps[current];
  input.estimated_bandwidth_bps = bandwidth_bps;
  input.buffered_duration = buffered;
  input.buffer_capacity = capacity;
  return input;
}

// ---- estimator -------------------------------------------------------------

TEST(AbrBandwidthEstimatorTest, SeedsFromTheFirstSample) {
  AbrBandwidthEstimator estimator;
  EXPECT_EQ(estimator.bits_per_second(), 0);
  estimator.OnSample(1000 * 1000, base::Seconds(1));  // 1 MB/s = 8 Mbps
  EXPECT_NEAR(estimator.bits_per_second(), 8000000, 80000);
}

TEST(AbrBandwidthEstimatorTest, EwmaConvergesTowardASustainedLevel) {
  AbrBandwidthEstimator estimator;
  for (int i = 0; i < 10; ++i) {
    estimator.OnSample(1000 * 1000, base::Seconds(1));  // 8 Mbps
  }
  // The link drops to 1 Mbps and stays there: after 20 samples the EWMA
  // must be well below the old level (reacted), though not fully there
  // (smoothed).
  for (int i = 0; i < 20; ++i) {
    estimator.OnSample(125 * 1000, base::Seconds(1));  // 1 Mbps
  }
  EXPECT_LT(estimator.bits_per_second(), 4000000);
  EXPECT_GT(estimator.bits_per_second(), 1000000);
}

TEST(AbrBandwidthEstimatorTest, IgnoresDegenerateSamples) {
  AbrBandwidthEstimator estimator;
  estimator.OnSample(0, base::Seconds(1));
  estimator.OnSample(1000, base::TimeDelta());
  EXPECT_EQ(estimator.bits_per_second(), 0);
  estimator.Reset();
  estimator.OnSample(1000 * 1000, base::Seconds(1));
  EXPECT_GT(estimator.bits_per_second(), 0);
}

// ---- algorithm --------------------------------------------------------------

TEST(AbrBufferAlgoTest, PanicsToTheFloorWhenTheBufferRunsDry) {
  AbrBufferAlgoStrategy algo;
  auto input = MakeInput({200000, 500000, 2000000}, 2, 10000000,
                         base::Milliseconds(100), base::Seconds(10));
  EXPECT_EQ(algo.Decide(input).target_index, 0);
}

TEST(AbrBufferAlgoTest, DropsWhenBandwidthCannotSustainTheRung) {
  AbrBufferAlgoStrategy algo;
  // Estimate 400k: above rung 0, below rung 1 -- land on rung 0.
  auto input = MakeInput({200000, 500000, 2000000}, 2, 400000, base::Seconds(5),
                         base::Seconds(10));
  EXPECT_EQ(algo.Decide(input).target_index, 0);
}

TEST(AbrBufferAlgoTest, ClimbsOnlyWithBufferAndBandwidthHeadroom) {
  AbrBufferAlgoStrategy algo;
  // Bandwidth affords it, buffer is at half capacity: climb one rung.
  auto climb = MakeInput({200000, 500000, 2000000}, 0, 1200000,
                         base::Seconds(5), base::Seconds(10));
  EXPECT_EQ(algo.Decide(climb).target_index, 1);
  // Same bandwidth, empty buffer: stay.
  auto hungry = MakeInput({200000, 500000, 2000000}, 0, 1200000,
                          base::Milliseconds(600), base::Seconds(10));
  EXPECT_EQ(algo.Decide(hungry).target_index, 0);
  // Bandwidth affords only 1x the next rung, not the 2x headroom: stay.
  auto marginal = MakeInput({200000, 500000, 2000000}, 0, 600000,
                            base::Seconds(5), base::Seconds(10));
  EXPECT_EQ(algo.Decide(marginal).target_index, 0);
}

TEST(AbrBufferAlgoTest, StaysAtTheTop) {
  AbrBufferAlgoStrategy algo;
  auto input = MakeInput({200000, 500000, 2000000}, 2, 10000000,
                         base::Seconds(5), base::Seconds(10));
  EXPECT_EQ(algo.Decide(input).target_index, 2);
}

}  // namespace
}  // namespace avbase::media

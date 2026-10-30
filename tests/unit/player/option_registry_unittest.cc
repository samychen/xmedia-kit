// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/public/option_registry.h"

#include "gtest/gtest.h"

namespace xmedia {
namespace {

const OptionRegistry& Registry() {
  return OptionRegistry::GetInstance();
}

// The four buffer keys were dead no-ops until BufferConfig was wired into
// BufferController / the demuxer queue / the PipelineImpl gate. These tests pin
// that the string keys now reach the typed fields (and back).

TEST(OptionRegistryGetFrameModeTest, SetsFieldAndDefaultsFalse) {
  PlayerConfig config;
  EXPECT_FALSE(config.get_frame_mode);
  EXPECT_TRUE(
      Registry()
          .SetString(&config, OptionCategory::kPlayer, "get-frame-mode", "1")
          .has_value());
  EXPECT_TRUE(config.get_frame_mode);
  EXPECT_TRUE(Registry()
                  .SetInt(&config, OptionCategory::kPlayer, "get-frame-mode", 0)
                  .has_value());
  EXPECT_FALSE(config.get_frame_mode);
}

TEST(OptionRegistryBufferTest, PacketBufferingSetsEnabled) {
  PlayerConfig config;
  // legacy passes "0"/"1"; accept both int and string forms.
  EXPECT_TRUE(
      Registry()
          .SetInt(&config, OptionCategory::kPlayer, "packet-buffering", 0)
          .has_value());
  EXPECT_FALSE(config.buffer.enabled);
  EXPECT_TRUE(
      Registry()
          .SetString(&config, OptionCategory::kPlayer, "packet-buffering", "1")
          .has_value());
  EXPECT_TRUE(config.buffer.enabled);
}

TEST(OptionRegistryBufferTest, InfbufSetsUnlimited) {
  PlayerConfig config;
  EXPECT_TRUE(Registry()
                  .SetInt(&config, OptionCategory::kPlayer, "infbuf", 1)
                  .has_value());
  EXPECT_TRUE(config.buffer.unlimited);
}

TEST(OptionRegistryBufferTest, MaxBufferSizeSetsBytes) {
  PlayerConfig config;
  EXPECT_TRUE(
      Registry()
          .SetInt(&config, OptionCategory::kPlayer, "max-buffer-size", 4096)
          .has_value());
  EXPECT_EQ(config.buffer.max_bytes, 4096u);
}

TEST(OptionRegistryBufferTest, MinFramesSetsFloor) {
  PlayerConfig config;
  EXPECT_TRUE(Registry()
                  .SetInt(&config, OptionCategory::kPlayer, "min-frames", 12)
                  .has_value());
  EXPECT_EQ(config.buffer.min_frames, 12u);
}

TEST(OptionRegistryBufferTest, OutOfRangeMaxBufferIsRejected) {
  PlayerConfig config;
  const Status s = Registry().SetInt(&config, OptionCategory::kPlayer,
                                     "max-buffer-size", -1);
  EXPECT_FALSE(s.has_value()) << "a negative byte cap must be refused";
}

TEST(OptionRegistryBufferTest, UnknownStringValueIsRejected) {
  PlayerConfig config;
  const Status s = Registry().SetString(&config, OptionCategory::kPlayer,
                                        "packet-buffering", "maybe");
  EXPECT_FALSE(s.has_value()) << "a non-boolean string must be refused";
}

TEST(OptionRegistryBufferTest, DumpRoundTripsTheKeys) {
  PlayerConfig config;
  const auto dump = Registry().Dump(config, OptionCategory::kPlayer);
  EXPECT_EQ(dump.count("packet-buffering"), 1u);
  EXPECT_EQ(dump.count("max-buffer-size"), 1u);
  EXPECT_EQ(dump.count("min-frames"), 1u);
  EXPECT_EQ(dump.count("infbuf"), 1u);
}

// ---- ValidateConfig: min-frames vs the packet-queue depth ------------------

TEST(ValidateConfigBufferTest, MinFramesBeyondQueueDepthIsFlagged) {
  PlayerConfig config;
  config.buffer.min_frames = 300;  // queue is 256 deep
  const std::vector<ConfigIssue> issues = ValidateConfig(config);
  bool found = false;
  for (const ConfigIssue& issue : issues) {
    if (issue.field == "buffer.min_frames") {
      found = true;
    }
  }
  EXPECT_TRUE(found) << "a min-frames floor the queue can never reach is a "
                        "deadlock and must be reported";
}

TEST(ValidateConfigBufferTest, MinFramesBeyondDepthAllowedWhenGateDisabled) {
  PlayerConfig config;
  config.buffer.enabled = false;  // packet-buffering off => gate never waits
  config.buffer.min_frames = 300;
  const std::vector<ConfigIssue> issues = ValidateConfig(config);
  for (const ConfigIssue& issue : issues) {
    EXPECT_NE(issue.field, "buffer.min_frames");
  }
}

TEST(ValidateConfigBufferTest, DefaultConfigHasNoIssues) {
  PlayerConfig config;
  EXPECT_TRUE(ValidateConfig(config).empty());
}

// ---- the two keys that were the last dead no-ops --------------------------
// Both write a field that now has a reader: nodisp reaches the renderer
// factory's sink selection, enable-accurate-seek is the veto SeekTo consults.
// The reader itself is asserted in default_renderer_factory_unittest.cc and
// player_seek_config_unittest.cc; here the question is only whether the string
// key reaches the field, which is the half that used to be silently absent.

TEST(OptionRegistryRenderTest, NoDispSetsDisableVideoOutput) {
  PlayerConfig config;
  EXPECT_TRUE(Registry()
                  .SetInt(&config, OptionCategory::kPlayer, "nodisp", 1)
                  .has_value());
  EXPECT_TRUE(config.render.disable_video_output);
  // "vn" is a different question (hide the stream) and must not move with it.
  EXPECT_FALSE(config.video.disabled);
  EXPECT_TRUE(Registry()
                  .SetString(&config, OptionCategory::kPlayer, "nodisp", "off")
                  .has_value());
  EXPECT_FALSE(config.render.disable_video_output);
}

TEST(OptionRegistrySeekTest, EnableAccurateSeekSetsTheFlag) {
  PlayerConfig config;
  EXPECT_TRUE(config.seek.accurate)
      << "the default must keep kAccurate working";
  EXPECT_TRUE(
      Registry()
          .SetInt(&config, OptionCategory::kPlayer, "enable-accurate-seek", 0)
          .has_value());
  EXPECT_FALSE(config.seek.accurate);
  // The timeout is a separate key and must survive its neighbour being set.
  EXPECT_FALSE(config.seek.accurate_timeout.is_negative());
}

TEST(OptionRegistrySeekTest, NonBooleanValueIsRejected) {
  PlayerConfig config;
  const bool before = config.seek.accurate;
  const Status s = Registry().SetString(&config, OptionCategory::kPlayer,
                                        "enable-accurate-seek", "sometimes");
  EXPECT_FALSE(s.has_value()) << "a non-boolean string must be refused";
  EXPECT_EQ(config.seek.accurate, before)
      << "a rejected value must leave the config untouched";
}

// "volume" was the one key docs/05 table 4 and MIGRATION Δ6 documented as
// "已接通（registry 换算）" while the registry had no entry for it at all:
// SetOption(kPlayer, "volume", 80) returned kInvalidArgument/"unknown option
// key". These pin the conversion in both directions and the rejection path.
TEST(OptionRegistryAudioTest, VolumeConvertsIjkPercentToFraction) {
  PlayerConfig config;
  EXPECT_TRUE(Registry()
                  .SetInt(&config, OptionCategory::kPlayer, "volume", 80)
                  .has_value());
  EXPECT_DOUBLE_EQ(config.audio.startup_volume, 0.8);
  // The boundaries, in the unit the old key used.
  EXPECT_TRUE(Registry()
                  .SetInt(&config, OptionCategory::kPlayer, "volume", 0)
                  .has_value());
  EXPECT_DOUBLE_EQ(config.audio.startup_volume, 0.0);
  EXPECT_TRUE(Registry()
                  .SetInt(&config, OptionCategory::kPlayer, "volume", 100)
                  .has_value());
  EXPECT_DOUBLE_EQ(config.audio.startup_volume, 1.0);
}

TEST(OptionRegistryAudioTest, VolumeAcceptsAnAlreadyFractionalDouble) {
  PlayerConfig config;
  EXPECT_TRUE(Registry()
                  .SetDouble(&config, OptionCategory::kPlayer, "volume", 0.35)
                  .has_value());
  EXPECT_DOUBLE_EQ(config.audio.startup_volume, 0.35);
}

TEST(OptionRegistryAudioTest, VolumeOutOfRangeIsRejectedAndLeavesTheField) {
  PlayerConfig config;
  config.audio.startup_volume = 0.5;
  EXPECT_FALSE(Registry()
                   .SetInt(&config, OptionCategory::kPlayer, "volume", 140)
                   .has_value());
  // 2.0 is out of range for the fraction form even though it would be a legal
  // percent -- the type says which unit the caller meant.
  EXPECT_FALSE(Registry()
                   .SetDouble(&config, OptionCategory::kPlayer, "volume", 2.0)
                   .has_value());
  EXPECT_DOUBLE_EQ(config.audio.startup_volume, 0.5)
      << "a rejected value must leave the config untouched";
}

}  // namespace
}  // namespace xmedia

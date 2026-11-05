// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The audio filter presets are strings; the assertions that matter are the
// ones a string-only test cannot fake: every preset graph BUILDS in the real
// FFmpegAudioFilter (the graph parser is the contract) and processes a silent
// buffer without error. The composition SDK's original preset set (volume
// drop / equalizer / echo / denoise / voice change) is covered one by one.

#include "player/public/audio_filter_presets.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "media/base/audio_buffer.h"
#include "media/ffmpeg/ffmpeg_audio_filter.h"

namespace xmedia {
using namespace media;
namespace {

base::scoped_refptr<AudioBuffer> Silence(int rate, int channels) {
  // 1024 frames of planar float zero — the decode path's own shape.
  // Planar float: channel planes concatenated in one flat vector (the
  // decode path's layout — channel_data() slices it per channel).
  std::vector<uint8_t> flat(
      static_cast<size_t>(channels) * 1024 * sizeof(float), 0);
  return AudioBuffer::Create(
      SampleFormat::kF32P, ChannelLayout::kStereo, channels, rate, 1024,
      base::TimeDelta(),
      base::SecondsD(static_cast<double>(1024) / rate), 0,
      std::move(flat));
}

// The real contract: the graph parses and the filter runs.
bool GraphWorks(const std::string& graph) {
  FFmpegAudioFilter filter;
  if (!filter.Initialize(graph, 48000, 2, 1024)) {
    return false;
  }
  std::vector<base::scoped_refptr<AudioBuffer>> out;
  return filter.Process(Silence(48000, 2), &out);
}

TEST(AudioFilterPresetsTest, EqualizerGraphBuildsAndProcesses) {
  const std::string graph =
      audio_filters::Equalizer({{100, 3.0}, {1000, -2.5}, {10000, 1.0}});
  EXPECT_NE(graph.find("equalizer=f=100"), std::string::npos);
  EXPECT_NE(graph.find("equalizer=f=10000"), std::string::npos);
  EXPECT_TRUE(GraphWorks(graph)) << graph;
}

TEST(AudioFilterPresetsTest, EqualizerClampsOutOfRangeBands) {
  // A 100 kHz centre would make the graph parser reject the biquad; the
  // preset clamps instead of handing the caller a broken graph.
  const std::string graph = audio_filters::Equalizer({{100000, 3.0}});
  EXPECT_TRUE(GraphWorks(graph)) << graph;
}

TEST(AudioFilterPresetsTest, EchoGraphBuildsAndProcesses) {
  const std::string graph =
      audio_filters::Echo(0.8, 0.9, {60, 120}, {0.4, 0.2});
  EXPECT_NE(graph.find("aecho="), std::string::npos);
  EXPECT_NE(graph.find("60|120"), std::string::npos);
  EXPECT_TRUE(GraphWorks(graph)) << graph;
}

TEST(AudioFilterPresetsTest, DenoiseGraphBuildsAndProcesses) {
  const std::string graph = audio_filters::Denoise(0.5);
  EXPECT_NE(graph.find("anlmdn"), std::string::npos);
  EXPECT_TRUE(GraphWorks(graph)) << graph;
}

TEST(AudioFilterPresetsTest, VoiceChangeGraphBuildsAndProcesses) {
  const std::string graph = audio_filters::VoiceChange(1.25, 48000);
  // asetrate carries the computed shifted rate (48000 * 1.25 = 60000), and
  // aresample pulls it back to the stream rate.
  EXPECT_NE(graph.find("asetrate=60000"), std::string::npos);
  EXPECT_NE(graph.find("aresample=48000"), std::string::npos);
  EXPECT_TRUE(GraphWorks(graph)) << graph;
}

TEST(AudioFilterPresetsTest, VoiceChangeChainsAtempoBeyondSingleStageRange) {
  // pitch 4.0 -> tempo 0.25, which a SINGLE atempo cannot express (its floor
  // is 0.5); the graph must chain two stages instead of emitting 0.25.
  const std::string graph = audio_filters::VoiceChange(4.0, 48000);
  EXPECT_NE(graph.find("atempo=0.5000,atempo=0.5000"), std::string::npos)
      << graph;
  EXPECT_EQ(graph.find("atempo=0.25"), std::string::npos) << graph;
  EXPECT_TRUE(GraphWorks(graph)) << graph;
}

}  // namespace
}  // namespace xmedia

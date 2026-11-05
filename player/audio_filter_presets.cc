// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/public/audio_filter_presets.h"

#include <algorithm>
#include <cmath>

namespace xmedia {
namespace audio_filters {
namespace {

std::string Number(double value, int precision = 2) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%.*f", precision, value);
  return std::string(buf);
}

}  // namespace

std::string Equalizer(const std::vector<std::pair<double, double>>& bands) {
  // One equalizer biquad per (centre Hz, gain dB) pair; Q=1.0 is the
  // composition SDK's default band width. Frequency clamps to the biquad's
  // usable range so a bad caller cannot produce an unparsable graph.
  std::string graph;
  for (const auto& band : bands) {
    const double f = std::clamp(band.first, 10.0, 20000.0);
    const double g = std::clamp(band.second, -30.0, 30.0);
    if (!graph.empty()) {
      graph += ",";
    }
    graph += "equalizer=f=" + Number(f, 1) + ":t=q:w=1:g=" + Number(g, 1);
  }
  return graph;
}

std::string Echo(double in_gain, double out_gain,
                 const std::vector<int>& delays_ms,
                 const std::vector<double>& decays) {
  // aecho takes at most four tap pairs; the gains are linear factors.
  std::vector<int> delays = delays_ms;
  std::vector<double> decays_v = decays;
  if (delays.empty()) {
    delays = {60};
    decays_v = {0.4};
  }
  delays.resize(std::min<size_t>(delays.size(), 4));
  decays_v.resize(std::min<size_t>(decays_v.size(), 4));
  while (decays_v.size() < delays.size()) {
    decays_v.push_back(decays_v.back() * 0.5);
  }
  std::string in = Number(std::clamp(in_gain, 0.0, 1.0));
  std::string out = Number(std::clamp(out_gain, 0.0, 1.0));
  std::string d, a;
  for (size_t i = 0; i < delays.size(); ++i) {
    if (i > 0) {
      d += "|";
      a += "|";
    }
    d += std::to_string(std::max(1, delays[i]));
    a += Number(std::clamp(decays_v[i], 0.0, 1.0), 3);
  }
  return "aecho=" + in + ":" + out + ":" + d + ":" + a;
}

std::string Denoise(double strength) {
  // anlmdn: |s| strength 0.001..1.0 (the composition SDK's 0..1 semantic
  // maps onto the filter's own range), preset s (medium).
  const double s = std::clamp(strength, 0.001, 1.0);
  return "anlmdn=s=" + Number(s, 3);
}

std::string VoiceChange(double pitch, int sample_rate) {
  // The classic voice-shift trio: asetrate moves the pitch (and the speed),
  // aresample pulls the rate back to the stream's, atempo restores the
  // original duration. asetrate takes a computed integer (not an
  // expression), and atempo's single-stage range is [0.5, 2.0] — a pitch
  // outside that needs a chained atempo per stage.
  const double p = std::clamp(pitch, 0.25, 4.0);
  const int shifted = std::max(1, static_cast<int>(sample_rate * p));
  std::string graph = "asetrate=" + std::to_string(shifted) +
                      ",aresample=" + std::to_string(sample_rate);
  // The duration-compensation factor is 1/p; each atempo stage can only
  // express [0.5, 2.0], so factor the remainder into as many stages as it
  // takes (chained atempo multiplies). A last stage shorter than a full
  // stage still clamps INSIDE the range — the tiny tempo error that buys
  // termination is inaudible and bounded by the clamp itself.
  // Multiplicative factorisation: chained atempo stages MULTIPLY, so the
  // remainder converges (R -> R/stage) for any positive R — a subtractive
  // loop here used to spin forever on the low-pitch side.
  double remaining = 1.0 / p;
  int stages = 0;
  while (std::abs(remaining - 1.0) > 0.005 && stages < 8) {
    const double stage = std::clamp(remaining, 0.5, 2.0);
    graph += ",atempo=" + Number(stage, 4);
    remaining /= stage;
    ++stages;
  }
  return graph;
}

}  // namespace audio_filters
}  // namespace xmedia

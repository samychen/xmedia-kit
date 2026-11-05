// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_PLAYER_PUBLIC_AUDIO_FILTER_PRESETS_H_
#define XMEDIA_PLAYER_PUBLIC_AUDIO_FILTER_PRESETS_H_

#include <string>
#include <utility>
#include <vector>

#include "player/public/player_export.h"


namespace xmedia {
namespace audio_filters {

// Named audio-effect presets rendered as FFmpeg filter-graph strings, ready
// for `PlayerConfig::audio.filter_graph` (the renderer's audio filter stage)
// or any AudioFilter construction site. Ported from the composition SDK's
// audio filter set (AudioFilterType_e: 降音量 / 均衡器 / 回声 / 降噪 / 变声),
// expressed with the native FFmpeg audio filters this build whitelists
// (aecho / equalizer / anlmdn / asetrate+atempo+aresample) instead of the
// sox/SoundTouch backends the composition SDK ships.
//
// Every function returns a FULL graph string (multiple filters chained with
// commas), parameter-validated: out-of-range arguments clamp rather than
// producing a graph the filter graph parser would reject.

// 多段均衡器：|bands| 为 (中心频率 Hz, 增益 dB) 对，每段一个 equalizer
// biquad；类型默认 double-pole wideband，宽度固定 Q=1.0（与合成 SDK 的默认
// band_width 对齐）。空表返回空串（无滤镜）。
XMEDIA_PLAYER_EXPORT std::string Equalizer(
    const std::vector<std::pair<double, double>>& bands);

// 回声：|in_gain|/|out_gain|（0..1 的线性增益，超过按 1 处理）、
// |delays_ms| 与 |decays| 等长（最多 4 段，aecho 的上限）。默认一对
// 60ms/0.4。
XMEDIA_PLAYER_EXPORT std::string Echo(double in_gain, double out_gain,
                                     const std::vector<int>& delays_ms,
                                     const std::vector<double>& decays);

// 降噪：anlmdn 的强度参数 |strength|（0.001..1.0，合成 SDK 的 0..1 语义原样
// 映射）；preset 固定 s（中档）。
XMEDIA_PLAYER_EXPORT std::string Denoise(double strength);

// 变声：|pitch| 为倍率（0.5 = 低八度感，2.0 = 高八度感），经典三件套
// asetrate(改变音高) + aresample(拉回原采样率) + atempo(补偿时长)。范围
// [0.25, 4.0]，越界钳制；|sample_rate| 是流的实际采样率（asetrate 要写
// 算好的新率，atempo 链式补偿到 atempo 的 [0.5,2.0] 单段上限内）。
XMEDIA_PLAYER_EXPORT std::string VoiceChange(double pitch,
                                             int sample_rate = 48000);

}  // namespace audio_filters
}  // namespace xmedia

#endif  // XMEDIA_PLAYER_PUBLIC_AUDIO_FILTER_PRESETS_H_

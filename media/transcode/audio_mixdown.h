// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_TRANSCODE_AUDIO_MIXDOWN_H_
#define XMEDIA_MEDIA_TRANSCODE_AUDIO_MIXDOWN_H_

// 确定性音频混音（HyperFrames audio 契约对齐）：音频/媒体元素按
// data-attributes.md 语义做文件级处理——解码源文件 → media-start 修剪 →
// volume 增益 → fade-in/out 线性斜坡 → 按 data-start 叠加进主缓冲。
//
// 为什么是文件级混音而不是浏览器实时捕获：bake 的确定性要求音频与视频
// 一样可重复——实时音频流跟随墙钟，逐帧 seek 驱动下不可复现；文件级处理
// 每次混音逐样本相同。原版管线同样是文件级（其打包 ffmpeg 侧处理）。

#include <cstdint>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "media/base/audio_buffer.h"
#include "media/media_export.h"

namespace xmedia {
namespace media {
namespace transcode {

// 一条待混音的音频/媒体 clip（来自采集元数据的音频词汇字段）。
struct XMEDIA_MEDIA_EXPORT AudioMixClip {
  std::string src;              // 音频/媒体文件路径（调用方已解析绝对路径）
  int64_t start_ms = 0;         // 合成时间线起点（data-start）
  int64_t duration_ms = -1;     // clip 时长（data-duration）；-1 = 到文件尾
  int64_t media_start_ms = 0;   // 源内修剪点（data-media-start）
  double volume = 1.0;          // data-volume（1 = 0dB）
  double fade_in = 0;           // data-fade-in（秒，线性斜坡）
  double fade_out = 0;          // data-fade-out（秒，线性斜坡）
};

// 混音结果：48kHz f32 平面立体声（AudioBuffer kF32P 的数据布局）。
struct XMEDIA_MEDIA_EXPORT AudioMixResult {
  std::vector<float> left;
  std::vector<float> right;
  int64_t sample_count = 0;  // 每通道样本数
};

// 将 |clips| 混入 |total_ms| 长度的主缓冲。master 按需扩容（clip 超出
// total 的尾部并入）。src 打不开或无音频流时跳过该 clip（日志 + 继续）。
bool XMEDIA_MEDIA_EXPORT MixAudioClips(const std::vector<AudioMixClip>& clips,
                                       int64_t total_ms, AudioMixResult* out);

// 主缓冲 → kF32P 立体声 AudioBuffer 序列（|frames_per_buffer| 每块，
// 时间戳从 0 起步进），供 FFmpegAudioEncoder::Encode 逐块消费。
std::vector<base::scoped_refptr<AudioBuffer>>
XMEDIA_MEDIA_EXPORT AudioMixToBuffers(const AudioMixResult& mix,
                                      int frames_per_buffer);

}  // namespace transcode
}  // namespace media
}  // namespace xmedia

#endif  // XMEDIA_MEDIA_TRANSCODE_AUDIO_MIXDOWN_H_

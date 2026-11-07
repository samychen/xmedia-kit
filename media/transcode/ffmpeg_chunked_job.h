// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_TRANSCODE_CHUNKED_JOB_H_
#define XMEDIA_MEDIA_TRANSCODE_CHUNKED_JOB_H_

#include <functional>
#include <string>

#include "media/base/media_error.h"
#include "media/media_export.h"
#include "media/transcode/ffmpeg_transcode_job.h"

namespace xmedia::media {

// Chunked transcode（HyperFrames 分块语义对齐）：把一条时间线按视频
// keyframe 网格切成 N 个时间窗，每窗独立 Transcode（编码器每块从关键帧
// 冷启，块间零依赖），再用 Concat 的拷贝路径无损拼接成最终输出。
//
// 为什么按 keyframe 对齐：Transcode 的 start 是"demuxer keyframe seek"，
// 非 keyframe 边界的窗会与前一窗重叠；把窗边界吸附到输入 keyframe 网格
// 上，每个窗的起点就是 seek 能精确落到的点，帧级 start 裁剪（见
// ffmpeg_transcode_job.cc 的 trim_start）保证不重叠。
//
// 与 HyperFrames 的 bake 分块的差别：那边每个块是全新合成渲染（任意
// 边界），这边是对已有输入的转码——能精确对齐的边界只有 keyframe，这是
// 输入侧的结构约束而非实现偷懒。
//
// 音频接缝：keyframe 是视频概念，音频窗起点落在两个音频帧之间时整帧
// 丢弃 → 接缝处至多一个音频编解码帧（48 kHz AAC 约 21 ms）的空隙。
// 视频块零缝隙。这不是对齐误差的掩盖——keyframe 网格下的最优解。
//
// 块文件是临时产物：<output>.chunk<i>.<ext>，Concat 成功后删除；中途
// 失败/取消也清理（输出目录需可写）。
struct XMEDIA_MEDIA_EXPORT ChunkedTranscodeParams {
  std::string input_uri;
  std::string output_path;
  // 每块的编解码规格（output_path/trim 字段被本模块覆写）。
  TranscodeParams transcode;
  // 目标块时长（秒）。<= 0 = 单块直通（等价于直接 Transcode）。
  double chunk_seconds = 10.0;
  // 容器格式；空 = 按扩展名推断（rtmp:// 等无扩展输出不支持分块，
  // 返回 kInvalidArgument）。
  std::string output_format;
};

using ChunkedProgressCB = std::function<void(int)>;  // 0..100

XMEDIA_MEDIA_EXPORT Status
ChunkedTranscode(const ChunkedTranscodeParams& params,
                 ChunkedProgressCB progress_cb = nullptr,
                 const TranscodeCancelToken* cancel = nullptr);

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_TRANSCODE_CHUNKED_JOB_H_

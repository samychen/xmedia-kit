// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Chunked transcode 驱动：keyframe 网格探查 → 逐窗 Transcode → Concat
// 拼接 → 清理。libav 只用于探查（keyframe 时间戳集合），编解码全部走
// 已有 Transcode/Concat 路径。

#include "media/transcode/ffmpeg_chunked_job.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "base/logging.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"
#include "media/transcode/ffmpeg_concat_job.h"

namespace xmedia::media {
namespace {

namespace ff = ::xmedia::media::ffmpeg;

// 输入视频流的 keyframe 时间戳（微秒，升序去重）。没有视频流的输入
// 返回空——无视频就无法定义 keyframe 网格，调用方退化为单块。
std::vector<int64_t> CollectKeyframeStamps(const std::string& uri,
                                           int64_t* total_duration_us) {
  *total_duration_us = 0;
  AVFormatContext* raw = nullptr;
  if (avformat_open_input(&raw, uri.c_str(), nullptr, nullptr) < 0) {
    return {};
  }
  ff::FormatCtxPtr ctx(raw);
  if (avformat_find_stream_info(ctx.get(), nullptr) < 0) {
    return {};
  }
  const int vidx =
      av_find_best_stream(ctx.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (ctx->duration > 0) {
    *total_duration_us = ctx->duration;  // AV_TIME_BASE 单位
  }

  std::vector<int64_t> stamps;
  if (vidx < 0) {
    return stamps;
  }
  ff::PacketPtr pkt = ff::MakePacket();
  while (av_read_frame(ctx.get(), pkt.get()) >= 0) {
    if (pkt->stream_index == vidx && (pkt->flags & AV_PKT_FLAG_KEY) &&
        pkt->pts != AV_NOPTS_VALUE) {
      const int64_t us = av_rescale_q(
          pkt->pts, ctx->streams[vidx]->time_base, AVRational{1, AV_TIME_BASE});
      if (stamps.empty() || stamps.back() != us) {
        stamps.push_back(us);
      }
    }
    av_packet_unref(pkt.get());
  }
  return stamps;
}

// 目标边界 t 吸附到 ≤ t 的最近 keyframe。
int64_t SnapToKeyframe(const std::vector<int64_t>& stamps, int64_t t_us) {
  const auto it = std::upper_bound(stamps.begin(), stamps.end(), t_us);
  if (it == stamps.begin()) {
    return stamps.front();
  }
  return *std::prev(it);
}

// 输出文件扩展名（含点；无扩展返回空）。
std::string OutputExtension(const std::string& path) {
  const auto slash = path.find_last_of('/');
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
    return {};
  }
  return path.substr(dot);  // ".mkv" 等
}

}  // namespace

Status ChunkedTranscode(const ChunkedTranscodeParams& params,
                        ChunkedProgressCB progress_cb,
                        const TranscodeCancelToken* cancel) {
  const auto cancelled = [cancel]() { return cancel && cancel->IsCancelled(); };

  const std::string ext = OutputExtension(params.output_path);
  if (ext.empty()) {
    return Err(ErrorCode::kInvalidArgument,
               "chunked transcode needs an extension-derivable container",
               params.output_path,
               "chunk files are named <output>.chunk<i>.<ext>; an rtmp:// or "
               "extensionless output cannot host them — use Transcode() for "
               "network outputs");
  }

  int64_t duration_us = 0;
  const std::vector<int64_t> keyframes =
      CollectKeyframeStamps(params.input_uri, &duration_us);
  if (keyframes.empty() && duration_us <= 0) {
    return Err(ErrorCode::kSourceOpenFailed, "cannot probe input for chunking",
               params.input_uri, "the file may be corrupt or unsupported");
  }

  // 窗边界：目标网格 i*chunk_seconds 吸附到 keyframe，去重 + 保证最后
  // 一窗到文件尾。keyframe 缺失（纯音频输入）退化为均匀网格——音频流
  // 的 seek 语义本就到帧，块接缝由帧级裁剪保证。
  std::vector<int64_t> edges;
  if (params.chunk_seconds <= 0.0 || keyframes.empty()) {
    edges = {0};  // 单块直通
  } else {
    const int64_t chunk_us =
        static_cast<int64_t>(params.chunk_seconds * AV_TIME_BASE);
    edges.push_back(0);
    for (int64_t t = chunk_us; t < duration_us; t += chunk_us) {
      const int64_t snapped = SnapToKeyframe(keyframes, t);
      if (snapped > edges.back()) {
        edges.push_back(snapped);
      }
    }
    if (edges.back() >= duration_us) {
      edges.pop_back();  // 最后一个边界越过 EOF：并回上一窗
    }
  }
  edges.push_back(duration_us > 0 ? duration_us : INT64_MAX);

  const size_t chunk_count = edges.size() - 1;
  std::vector<std::string> chunk_paths;
  chunk_paths.reserve(chunk_count);

  const auto fail = [&](const Status& st, const char* what) {
    // 半成品块对调用方没有价值；完整文件才删（Concat 失败时块文件
    // 一起删——输出目录是块文件与最终输出的共同前提）。
    for (const auto& path : chunk_paths) {
      std::remove(path.c_str());
    }
    LOG(ERROR) << "chunked transcode: " << what << ": " << st.error().ToString();
    return st;
  };

  for (size_t i = 0; i < chunk_count; ++i) {
    if (cancelled()) {
      return fail(Err(ErrorCode::kCancelled, "chunked transcode cancelled",
                      params.input_uri,
                      "the caller cancelled the job; partial chunk files are "
                      "removed"),
                  "cancelled");
    }
    TranscodeParams p = params.transcode;
    p.output_path = params.output_path + ".chunk" + std::to_string(i) + ext;
    p.output_format = params.output_format;
    p.start_time_seconds =
        static_cast<double>(edges[i]) / AV_TIME_BASE;
    p.duration_seconds = edges[i + 1] >= INT64_MAX
                             ? 0.0
                             : static_cast<double>(edges[i + 1] - edges[i]) /
                                   AV_TIME_BASE;
    const Status st = Transcode(params.input_uri, p, nullptr, cancel);
    if (!st) {
      return fail(st, "a chunk failed to transcode");
    }
    chunk_paths.push_back(std::move(p.output_path));
    if (progress_cb) {
      progress_cb(static_cast<int>((i + 1) * 100 / chunk_count));
    }
  }

  ConcatParams cp;
  cp.inputs = chunk_paths;
  cp.output_path = params.output_path;
  cp.output_format = params.output_format;
  cp.muxer_options = params.transcode.muxer_options;
  const Status st = Concat(cp);
  if (!st) {
    return fail(st, "concat failed");
  }
  for (const auto& path : chunk_paths) {
    std::remove(path.c_str());
  }
  return OkStatus();
}

}  // namespace xmedia::media

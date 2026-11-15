// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_EXAMPLES_VIDEO_ELEMENT_SOURCE_H_
#define XMEDIA_EXAMPLES_VIDEO_ELEMENT_SOURCE_H_

// 视频元素的原生解码源：html_bake 的 <video> 元素像素由 FFmpeg 直接解
// 码——CEF OSR 不把 <video> 合成进 OnPaint 位图（实测冻结），页面里的
// video 元素只是元数据（窗口/音量），像素走 GlScene 独立层。

#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xmedia {
namespace examples {

class VideoElementSource {
 public:
  VideoElementSource() = default;
  ~VideoElementSource() { Close(); }
  VideoElementSource(const VideoElementSource&) = delete;
  VideoElementSource& operator=(const VideoElementSource&) = delete;
  VideoElementSource(VideoElementSource&& o) noexcept
      : fmt_(o.fmt_), dec_(o.dec_), frame_(o.frame_), pkt_(o.pkt_),
        sws_(o.sws_), stream_(o.stream_), w_(o.w_), h_(o.h_),
        duration_(o.duration_), frame_dur_(o.frame_dur_),
        last_pts_(o.last_pts_), eos_(o.eos_), bgra_(std::move(o.bgra_)) {
    o.fmt_ = nullptr;
    o.dec_ = nullptr;
    o.frame_ = nullptr;
    o.pkt_ = nullptr;
    o.sws_ = nullptr;
    o.stream_ = -1;
    o.w_ = o.h_ = 0;
    o.last_pts_ = -1e9;
    o.eos_ = false;
  }
  VideoElementSource& operator=(VideoElementSource&& o) noexcept {
    if (this != &o) {
      Close();
      fmt_ = o.fmt_;
      dec_ = o.dec_;
      frame_ = o.frame_;
      pkt_ = o.pkt_;
      sws_ = o.sws_;
      stream_ = o.stream_;
      w_ = o.w_;
      h_ = o.h_;
      duration_ = o.duration_;
      frame_dur_ = o.frame_dur_;
      last_pts_ = o.last_pts_;
      eos_ = o.eos_;
      bgra_ = std::move(o.bgra_);
      o.fmt_ = nullptr;
      o.dec_ = nullptr;
      o.frame_ = nullptr;
      o.pkt_ = nullptr;
      o.sws_ = nullptr;
      o.stream_ = -1;
      o.w_ = o.h_ = 0;
      o.last_pts_ = -1e9;
      o.eos_ = false;
    }
    return *this;
  }

  bool Open(const std::string& path);
  // 取视频内 |sec|（局部时刻）的帧 → BGRA。顺序推进零解码开销；回跳
  // 自动 seek。返回 false = 无帧可给（调用方保留上一帧）。
  bool FrameAt(double sec);
  void Close();

  const std::vector<uint8_t>& bgra() const { return bgra_; }
  int width() const { return w_; }
  int height() const { return h_; }
  double duration() const { return duration_; }

 private:
  bool DecodeNext(double until_sec);

  AVFormatContext* fmt_ = nullptr;
  AVCodecContext* dec_ = nullptr;
  AVFrame* frame_ = nullptr;
  AVPacket* pkt_ = nullptr;
  struct SwsContext* sws_ = nullptr;
  int stream_ = -1;
  int w_ = 0;
  int h_ = 0;
  double duration_ = 0;
  double frame_dur_ = 1.0 / 30.0;
  double last_pts_ = -1e9;
  bool eos_ = false;
  std::vector<uint8_t> bgra_;
};

}  // namespace examples
}  // namespace xmedia

#endif  // XMEDIA_EXAMPLES_VIDEO_ELEMENT_SOURCE_H_

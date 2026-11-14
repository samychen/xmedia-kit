// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "video_element_source.h"

#include <libswscale/swscale.h>

extern "C" {
// 本构建的 libavutil/hash.h 无 C++ 守卫——同款处理前置（若将来引入）。
}

#include "base/logging.h"

namespace xmedia {
namespace examples {

bool VideoElementSource::Open(const std::string& path) {
  Close();
  if (avformat_open_input(&fmt_, path.c_str(), nullptr, nullptr) < 0 ||
      !fmt_) {
    LOG(WARNING) << "video element: cannot open " << path;
    return false;
  }
  if (avformat_find_stream_info(fmt_, nullptr) < 0) {
    Close();
    return false;
  }
  stream_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (stream_ < 0) {
    LOG(WARNING) << "video element: no video stream in " << path;
    Close();
    return false;
  }
  AVStream* st = fmt_->streams[stream_];
  const AVCodec* dec = avcodec_find_decoder(st->codecpar->codec_id);
  if (!dec) {
    LOG(WARNING) << "video element: no decoder for codec "
                 << st->codecpar->codec_id;
    Close();
    return false;
  }
  dec_ = avcodec_alloc_context3(dec);
  if (!dec_ || avcodec_parameters_to_context(dec_, st->codecpar) < 0 ||
      avcodec_open2(dec_, dec, nullptr) < 0) {
    Close();
    return false;
  }
  frame_ = av_frame_alloc();
  pkt_ = av_packet_alloc();
  w_ = dec_->width;
  h_ = dec_->height;
  duration_ = st->duration > 0
                  ? st->duration * av_q2d(st->time_base)
                  : (fmt_->duration > 0 ? fmt_->duration /
                                          static_cast<double>(AV_TIME_BASE)
                                        : 0);
  AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate
                                         : st->r_frame_rate;
  if (fr.num > 0) {
    frame_dur_ = av_q2d(av_inv_q(fr));
  }
  bgra_.resize(static_cast<size_t>(w_) * h_ * 4);
  return true;
}

bool VideoElementSource::DecodeNext(double until_sec) {
  // 解码直到拿到 pts >= until_sec 的帧（或 EOF），期间刷新缓存帧。
  while (!eos_) {
    if (av_read_frame(fmt_, pkt_) < 0) {
      eos_ = true;
      avcodec_send_packet(dec_, nullptr);  // 冲刷尾部
      break;
    }
    if (pkt_->stream_index != stream_) {
      av_packet_unref(pkt_);
      continue;
    }
    if (avcodec_send_packet(dec_, pkt_) < 0) {
      av_packet_unref(pkt_);
      continue;
    }
    av_packet_unref(pkt_);
    while (avcodec_receive_frame(dec_, frame_) >= 0) {
      double pts = frame_->best_effort_timestamp > 0
                       ? frame_->best_effort_timestamp *
                             av_q2d(fmt_->streams[stream_]->time_base)
                       : last_pts_ + frame_dur_;
      // 换算 BGRA（首帧后建 sws）。
      if (!sws_) {
        sws_ = sws_getContext(w_, h_, static_cast<AVPixelFormat>(frame_->format),
                              w_, h_, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr,
                              nullptr, nullptr);
        if (!sws_) {
          return false;
        }
      }
      std::vector<uint8_t> out(static_cast<size_t>(w_) * h_ * 4);
      uint8_t* dst[4] = {out.data(), nullptr, nullptr, nullptr};
      int ls[4] = {w_ * 4, 0, 0, 0};
      sws_scale(sws_, frame_->data, frame_->linesize, 0, h_, dst, ls);
      bgra_.swap(out);
      w_ = frame_->width;
      h_ = frame_->height;
      last_pts_ = pts;
      if (pts + frame_dur_ > until_sec) {
        return true;  // 该帧的展示区间覆盖 until_sec
      }
      // 还没到目标时刻：继续解下一帧（保留缓存帧为当前最新）
    }
  }
  // EOF：缓存帧已到片尾，视为覆盖
  return last_pts_ >= 0;
}

bool VideoElementSource::FrameAt(double sec) {
  if (!dec_) {
    return false;
  }
  if (sec < last_pts_) {
    // 回跳：seek 到目标前再顺解。
    AVStream* st = fmt_->streams[stream_];
    int64_t ts = static_cast<int64_t>(sec / av_q2d(st->time_base));
    av_seek_frame(fmt_, stream_, ts, AVSEEK_FLAG_BACKWARD);
    avcodec_flush_buffers(dec_);
    last_pts_ = -1e9;
    eos_ = false;
  }
  return DecodeNext(sec);
}

void VideoElementSource::Close() {
  if (sws_) {
    sws_freeContext(sws_);
    sws_ = nullptr;
  }
  if (pkt_) {
    av_packet_free(&pkt_);
  }
  if (frame_) {
    av_frame_free(&frame_);
  }
  if (dec_) {
    avcodec_free_context(&dec_);
    dec_ = nullptr;
  }
  if (fmt_) {
    avformat_close_input(&fmt_);
  }
  stream_ = -1;
  w_ = h_ = 0;
  last_pts_ = -1e9;
  eos_ = false;
  bgra_.clear();
}

}  // namespace examples
}  // namespace xmedia

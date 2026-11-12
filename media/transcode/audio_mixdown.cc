// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// 确定性音频混音实现：libavformat/libavcodec 解码源文件 → swresample 归一
// 到 48kHz f32 平面立体声 → 修剪/增益/斜坡 → 叠加。libav 直接使用（同
// encode_verify 模式；确定性 = 纯样本数学，无墙钟）。

#include <map>
#include <set>
#include <type_traits>

#include "media/transcode/audio_mixdown.h"

#include <algorithm>
#include <cmath>

#include "base/logging.h"
#include "media/ffmpeg/audio_convert.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"

namespace xmedia {
namespace media {
namespace transcode {

namespace ff = ::xmedia::media::ffmpeg;

namespace {

constexpr int kOutRate = 48000;
constexpr int kOutChannels = 2;

// 单文件解码 + swr 归一 → FLTP 48k 立体声样本序列。
bool DecodeToStereo48k(const std::string& path,
                       std::vector<float>* left, std::vector<float>* right) {
  AVFormatContext* raw = nullptr;
  if (avformat_open_input(&raw, path.c_str(), nullptr, nullptr) < 0) {
    LOG(WARNING) << "audio mixdown: cannot open " << path;
    return false;
  }
  ff::FormatCtxPtr fmt(raw);
  if (avformat_find_stream_info(fmt.get(), nullptr) < 0) {
    LOG(WARNING) << "audio mixdown: cannot probe " << path;
    return false;
  }
  const int aidx =
      av_find_best_stream(fmt.get(), AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
  if (aidx < 0) {
    LOG(WARNING) << "audio mixdown: no audio stream in " << path;
    return false;
  }
  AVStream* as = fmt->streams[static_cast<unsigned>(aidx)];
  const AVCodec* dec = avcodec_find_decoder(as->codecpar->codec_id);
  if (dec == nullptr) {
    LOG(WARNING) << "audio mixdown: no decoder for codec "
                 << as->codecpar->codec_id;
    return false;
  }
  ff::CodecCtxPtr dec_ctx(avcodec_alloc_context3(dec));
  if (!dec_ctx ||
      avcodec_parameters_to_context(dec_ctx.get(), as->codecpar) < 0 ||
      avcodec_open2(dec_ctx.get(), dec, nullptr) < 0) {
    LOG(WARNING) << "audio mixdown: cannot open decoder for " << path;
    return false;
  }
  dec_ctx->pkt_timebase = as->time_base;

  // AudioConverter（项目 swr 封装）：任意源 → FLTP 48k 立体声。
  const int in_channels =
      dec_ctx->ch_layout.nb_channels > 0 ? dec_ctx->ch_layout.nb_channels : 2;
  ff::AudioConverter conv;
  if (!conv.Configure(dec_ctx->sample_rate, in_channels,
                      static_cast<AVSampleFormat>(dec_ctx->sample_fmt),
                      kOutRate, kOutChannels, AV_SAMPLE_FMT_FLTP)) {
    LOG(WARNING) << "audio mixdown: converter configure failed for " << path;
    return false;
  }

  auto append_frame = [&](const AVFrame& f) {
    const size_t n = static_cast<size_t>(f.nb_samples);
    // FLTP 平面：data[0]=L, data[1]=R。
    const auto* l = reinterpret_cast<const float*>(f.data[0]);
    const auto* r = reinterpret_cast<const float*>(f.data[1]);
    left->insert(left->end(), l, l + n);
    right->insert(right->end(), r, r + n);
  };

  ff::PacketPtr pkt = ff::MakePacket();
  ff::FramePtr frame = ff::MakeFrame();
  while (av_read_frame(fmt.get(), pkt.get()) >= 0) {
    if (pkt->stream_index != aidx) {
      av_packet_unref(pkt.get());
      continue;
    }
    if (avcodec_send_packet(dec_ctx.get(), pkt.get()) >= 0) {
      while (avcodec_receive_frame(dec_ctx.get(), frame.get()) >= 0) {
        for (const auto& out : conv.Push(*frame)) {
          append_frame(*out);
        }
      }
    }
    av_packet_unref(pkt.get());
  }
  avcodec_send_packet(dec_ctx.get(), nullptr);
  while (avcodec_receive_frame(dec_ctx.get(), frame.get()) >= 0) {
    for (const auto& out : conv.Push(*frame)) {
      append_frame(*out);
    }
  }
  for (const auto& out : conv.Flush()) {
    append_frame(*out);
  }
  return !left->empty();
}

// 区段线性斜坡增益（fade_in 从 clip 起点升到 1；fade_out 从终点降回 0）。
double RampGain(int64_t sample_index, int64_t clip_start_sample,
                int64_t clip_end_sample, double fade_in, double fade_out) {
  double gain = 1.0;
  const int64_t in_samples = static_cast<int64_t>(fade_in * kOutRate);
  const int64_t out_samples = static_cast<int64_t>(fade_out * kOutRate);
  const int64_t rel = sample_index - clip_start_sample;
  if (in_samples > 0 && rel < in_samples) {
    gain *= static_cast<double>(rel) / static_cast<double>(in_samples);
  }
  const int64_t from_end = clip_end_sample - sample_index;
  if (out_samples > 0 && rel >= 0 && from_end < out_samples) {
    gain *= static_cast<double>(from_end) / static_cast<double>(out_samples);
  }
  return std::clamp(gain, 0.0, 1.0);
}

}  // namespace

bool MixAudioClips(const std::vector<AudioMixClip>& clips, int64_t total_ms,
                   AudioMixResult* out) {
  if (out == nullptr || total_ms <= 0) {
    return false;
  }
  const int64_t master_samples = total_ms * kOutRate / 1000;
  out->left.assign(static_cast<size_t>(master_samples), 0.f);
  out->right.assign(static_cast<size_t>(master_samples), 0.f);
  out->sample_count = master_samples;

  int used = 0;
  for (const AudioMixClip& clip : clips) {
    if (clip.src.empty() || clip.start_ms >= total_ms) {
      continue;
    }
    std::vector<float> left;
    std::vector<float> right;
    if (!DecodeToStereo48k(clip.src, &left, &right)) {
      continue;
    }
    ++used;
    const int64_t media_start_sample =
        clip.media_start_ms * kOutRate / 1000;
    int64_t src_pos = media_start_sample;
    int64_t clip_len = clip.duration_ms >= 0
                           ? clip.duration_ms * kOutRate / 1000
                           : (static_cast<int64_t>(left.size()) - src_pos);
    if (clip.duration_ms < 0) {
      clip_len = static_cast<int64_t>(left.size()) - src_pos;
    }
    const int64_t dst_begin = clip.start_ms * kOutRate / 1000;
    const double vol = clip.volume;
    for (int64_t i = 0; i < clip_len; ++i) {
      const int64_t dst = dst_begin + i;
      if (dst < 0 || dst >= master_samples) {
        break;
      }
      const int64_t s = src_pos + i;
      if (s < 0 || static_cast<size_t>(s) >= left.size()) {
        break;
      }
      const double g =
          RampGain(dst, dst_begin, dst_begin + clip_len, clip.fade_in,
                   clip.fade_out) * vol;
      out->left[static_cast<size_t>(dst)] +=
          static_cast<float>(left[static_cast<size_t>(s)] * g);
      out->right[static_cast<size_t>(dst)] +=
          static_cast<float>(right[static_cast<size_t>(s)] * g);
    }
    src_pos += clip_len;
  }
  if (used == 0) {
    LOG(WARNING) << "audio mixdown: no usable audio clips";
  }
  return true;
}

std::vector<base::scoped_refptr<AudioBuffer>> AudioMixToBuffers(
    const AudioMixResult& mix, int frames_per_buffer) {
  std::vector<base::scoped_refptr<AudioBuffer>> out;
  const int64_t total = mix.sample_count;
  int64_t pos = 0;
  while (pos < total) {
    const int64_t n = std::min<int64_t>(frames_per_buffer, total - pos);
    std::vector<uint8_t> data(static_cast<size_t>(n) * 2 * sizeof(float));
    auto* dst = reinterpret_cast<float*>(data.data());
    for (int64_t i = 0; i < n; ++i) {
      dst[i] = mix.left[static_cast<size_t>(pos + i)];
      dst[n + i] = mix.right[static_cast<size_t>(pos + i)];
    }
    auto buf = AudioBuffer::Create(
        SampleFormat::kF32P, ChannelLayout::kStereo, 2, kOutRate,
        static_cast<int>(n),
        base::Microseconds(pos * 1000000 / kOutRate),
        base::Microseconds(n * 1000000 / kOutRate), 0, std::move(data));
    if (buf) {
      out.push_back(std::move(buf));
    }
    pos += n;
  }
  return out;
}

}  // namespace transcode
}  // namespace media
}  // namespace xmedia

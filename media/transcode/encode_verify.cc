// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// T9b verify-seek implementation: libav direct (the playback decoder stack is
// a player-oriented graph; verification needs exactly one seek + decode +
// sws per sample, which is a dozen libav calls, not a pipeline).

#include "media/transcode/encode_verify.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/logging.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"

namespace xmedia::media::dedup {
namespace {

namespace ff = ::xmedia::media::ffmpeg;

struct FmtCloser {
  void operator()(AVFormatContext* c) const {
    if (c) {
      avformat_close_input(&c);
    }
  }
};
struct CodecCloser {
  void operator()(AVCodecContext* c) const {
    if (c) {
      avcodec_free_context(&c);
    }
  }
};
struct SwsCloser {
  void operator()(SwsContext* c) const {
    if (c) {
      sws_freeContext(c);
    }
  }
};

// One decoded-back sample: converts |frame| (any decoder output format) to
// YUV420P and computes its luma grid. |rec.pts_ms| is preserved from |pts_ms|.
bool SampleFromFrame(const AVFrame* frame, int64_t pts_ms,
                     FrameVerifyRecord* rec) {
  if (frame->width <= 0 || frame->height <= 0) {
    return false;
  }
  std::unique_ptr<SwsContext, SwsCloser> sws(sws_getContext(
      frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
      frame->width, frame->height, AV_PIX_FMT_YUV420P, SWS_BILINEAR, nullptr,
      nullptr, nullptr));
  if (!sws) {
    return false;
  }
  ff::FramePtr dst = ff::MakeFrame();
  dst->format = AV_PIX_FMT_YUV420P;
  dst->width = frame->width;
  dst->height = frame->height;
  if (av_frame_get_buffer(dst.get(), 32) < 0) {
    return false;
  }
  // sws_scale takes non-const src pointers; the decoder frame is not mutated.
  if (sws_scale(sws.get(), frame->data, frame->linesize, 0, frame->height,
                dst->data, dst->linesize) < 0) {
    return false;
  }
  rec->pts_ms = pts_ms;
  return ComputeYuvLumaSignature(dst->data[0], dst->linesize[0], dst->width,
                                 dst->height, rec);
}

}  // namespace

bool VerifyEncodedOutput(const std::string& output_path,
                         const std::vector<FrameVerifyRecord>& input_records,
                         double fps, const EncodeVerifyParams& params,
                         EncodeVerifyReport* report) {
  if (report == nullptr || input_records.empty() || output_path.empty()) {
    return false;
  }
  *report = EncodeVerifyReport();

  AVFormatContext* raw_fmt = nullptr;
  if (avformat_open_input(&raw_fmt, output_path.c_str(), nullptr, nullptr) <
      0) {
    LOG(WARNING) << "encode_verify: cannot open output for readback";
    return false;
  }
  std::unique_ptr<AVFormatContext, FmtCloser> fmt(raw_fmt);
  if (avformat_find_stream_info(fmt.get(), nullptr) < 0) {
    LOG(WARNING) << "encode_verify: cannot probe output streams";
    return false;
  }
  const int vidx =
      av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (vidx < 0) {
    LOG(WARNING) << "encode_verify: output has no video stream";
    return false;
  }
  AVStream* vs = fmt->streams[static_cast<unsigned>(vidx)];
  const AVCodec* dec_codec = avcodec_find_decoder(vs->codecpar->codec_id);
  if (dec_codec == nullptr) {
    LOG(WARNING) << "encode_verify: no decoder for output codec";
    return false;
  }
  std::unique_ptr<AVCodecContext, CodecCloser> dec(
      avcodec_alloc_context3(dec_codec));
  if (!dec || avcodec_parameters_to_context(dec.get(), vs->codecpar) < 0 ||
      avcodec_open2(dec.get(), dec_codec, nullptr) < 0) {
    LOG(WARNING) << "encode_verify: cannot open output decoder";
    return false;
  }
  dec->pkt_timebase = vs->time_base;

  // Evenly subsample the input records down to |sample_count|.
  std::vector<FrameVerifyRecord> samples;
  const int want = params.sample_count > 0
                       ? std::min<int>(params.sample_count,
                                       static_cast<int>(input_records.size()))
                       : static_cast<int>(input_records.size());
  for (int i = 0; i < want; ++i) {
    const size_t idx = static_cast<size_t>(input_records.size()) * i / want;
    samples.push_back(input_records[idx]);
  }

  ff::PacketPtr pkt = ff::MakePacket();
  // Half a frame of slack: a decoded frame "at" the target may land a few
  // ms early depending on container rounding.
  const double eps_ms = fps > 0.0 ? 500.0 / fps : 0.0;

  for (const FrameVerifyRecord& sample : samples) {
    ++report->samples_checked;
    const int64_t target_ts =
        av_rescale_q(sample.pts_ms, {1, 1000}, vs->time_base);
    // BACKWARD lands on the keyframe at/before the target; decoding forward
    // from there reaches the exact frame.
    if (av_seek_frame(fmt.get(), vidx, target_ts, AVSEEK_FLAG_BACKWARD) < 0) {
      report->failed_frame = sample.frame_index;
      break;
    }
    avcodec_flush_buffers(dec.get());

    bool got = false;
    bool mismatch = false;
    while (!got && !mismatch) {
      if (av_read_frame(fmt.get(), pkt.get()) < 0) {
        break;  // EOF before the target frame: sample failed.
      }
      if (pkt->stream_index != vidx) {
        av_packet_unref(pkt.get());
        continue;
      }
      const int sent = avcodec_send_packet(dec.get(), pkt.get());
      av_packet_unref(pkt.get());
      if (sent < 0 && sent != AVERROR(EAGAIN)) {
        break;
      }
      while (true) {
        ff::FramePtr fr = ff::MakeFrame();
        const int rret = avcodec_receive_frame(dec.get(), fr.get());
        if (rret < 0) {
          break;  // EAGAIN / EOF: need more packets before the target.
        }
        const int64_t frame_ms =
            fr->pts != AV_NOPTS_VALUE
                ? av_rescale_q(fr->pts, dec->pkt_timebase, {1, 1000})
                : AV_NOPTS_VALUE;
        if (frame_ms == AV_NOPTS_VALUE || frame_ms + eps_ms < sample.pts_ms) {
          continue;  // still before the target frame.
        }
        FrameVerifyRecord out_rec;
        if (!SampleFromFrame(fr.get(), frame_ms, &out_rec)) {
          break;
        }
        const double rms =
            CompareFrameVerifyRecords(sample, out_rec, params.tolerance_rms)
                .rms_diff;
        report->worst_rms = std::max(report->worst_rms, rms);
        report->output_records.push_back(std::move(out_rec));
        got = true;
        if (rms > params.tolerance_rms) {
          mismatch = true;
          report->failed_frame = sample.frame_index;
        }
        break;
      }
    }
    if (!got || mismatch) {
      report->failed_frame = sample.frame_index;
      break;
    }
  }

  // passed is derived, not accumulated: every sample reached and compared
  // within tolerance leaves failed_frame at its sentinel -1.
  report->passed = report->failed_frame < 0 && report->samples_checked > 0;
  return true;
}

std::string SerializeVerifySidecar(
    double fps, int64_t frame_count,
    const std::vector<FrameVerifyRecord>& output_records,
    const EncodeVerifyReport& report) {
  std::string json = SerializeFrameVerifyRecords(kFrameVerifyGridW,
                                                 kFrameVerifyGridH, fps,
                                                 frame_count, output_records);
  char buf[128];
  snprintf(buf, sizeof(buf),
           "\"verify\":{\"passed\":%s,\"worst_rms\":%.3f,"
           "\"failed_frame\":%lld,\"samples\":%d}",
           report.passed ? "true" : "false", report.worst_rms,
           static_cast<long long>(report.failed_frame), report.samples_checked);
  const size_t pos = json.rfind('}');
  if (pos != std::string::npos) {
    json.insert(pos, std::string(",") + buf);
  }
  return json;
}

}  // namespace xmedia::media::dedup

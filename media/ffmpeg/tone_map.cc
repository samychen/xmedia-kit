// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/tone_map.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "base/logging.h"
#include "media/ffmpeg/compat.h"

namespace xmedia::media::ffmpeg {
const char* ToneMapCurveName(ToneMapCurve curve) {
  switch (curve) {
  case ToneMapCurve::kReinhard:
    return "reinhard";
  case ToneMapCurve::kMobius:
    return "mobius";
  case ToneMapCurve::kHable:
    return "hable";
  }
  return "hable";
}

namespace {

// The graph every mapped frame runs through. Written as one string with the
// two tunable values substituted, because avfilter_graph_parse_ptr takes a
// whole chain and splitting it across calls would only move the same text
// around.
//
// Each stage says something a reader will otherwise have to guess at:
//   zscale t=linear   : decode PQ/HLG into linear light. Tonemapping in a
//                       non-linear encoding is meaningless -- "half the
//                       luminance" is not "half the code value".
//   npl=<peak>        : the source's nominal peak, so zscale normalises before
//                       the curve compresses. Without it an HLG stream whose
//                       content peaks at 1000 is normalised as if it were
//                       10000 and comes out dim.
//   tonemap=<curve>   : the compression itself.
//   desat=<k>         : out-of-gamut colours are pulled toward white as they
//                       are compressed. 2.0 (the filter default) is too strong
//                       for skin tones; 0 leaves neon edges.
//   zscale t=bt709 m=bt709 p=bt709 r=tv : encode for an SDR BT.709 display,
//                       limited range, which is what the renderer's YUV->RGB
//                       matrix assumes.
std::string BuildGraphString(ToneMapCurve curve, double peak_luminance,
                             double desat) {
  // One source of truth for the spelling: it is also what the diagnostics
  // line prints, and two copies would let the graph and the message disagree
  // about which curve is running.
  const char* curve_name = ToneMapCurveName(curve);
  char peak[64];
  if (peak_luminance > 0.0) {
    snprintf(peak, sizeof(peak), ":npl=%.3f", peak_luminance);
  } else {
    // No mastering-display metadata: let zscale use the transfer function's
    // own default rather than inventing a number here.
    peak[0] = '\0';
  }
  char buffer[512];
  snprintf(buffer, sizeof(buffer),
           "zscale=t=linear%s,tonemap=tonemap=%s:desat=%.3f,"
           "zscale=t=bt709:m=bt709:p=bt709:r=tv,format=yuv420p",
           peak, curve_name, desat);
  return buffer;
}

}  // namespace

bool IsHdrTransfer(int av_transfer) {
  return av_transfer == AVCOL_TRC_SMPTE2084 ||
         av_transfer == AVCOL_TRC_ARIB_STD_B67;
}

struct ToneMapper::Graph {
  AVFilterGraph* graph = nullptr;
  AVFilterContext* src = nullptr;
  AVFilterContext* sink = nullptr;
  int width = 0;
  int height = 0;
};

ToneMapper::ToneMapper() = default;

ToneMapper::~ToneMapper() = default;

bool ToneMapper::IsAvailable() {
  return avfilter_get_by_name("zscale") != nullptr &&
         avfilter_get_by_name("tonemap") != nullptr;
}

void ToneMapper::Reset() {
  graph_.reset();
  filter_graph_.clear();
}

bool ToneMapper::Configure(int width, int height, int av_transfer,
                           ToneMapCurve curve, double peak_luminance,
                           double desat) {
  if (!IsAvailable()) {
    LOG(WARNING) << "tone map: this FFmpeg build has no zscale/tonemap "
                    "filter; HDR will be shown without tone mapping";
    return false;
  }
  if (!IsHdrTransfer(av_transfer)) {
    // Refusing is the safe answer: tone mapping SDR content crushes its
    // blacks, and a caller that asks for it has a bug.
    return false;
  }
  if (width <= 0 || height <= 0) {
    return false;
  }
  // Clamped rather than rejected: 0 makes compressed highlights bleed into
  // their neighbours, and >1 is not a saturation any codec expects.
  const double clamped_desat = std::clamp(desat, 0.0, 1.0);
  filter_graph_ = BuildGraphString(curve, peak_luminance, clamped_desat);
  // The graph itself is built lazily in Process(): it needs the frame's pixel
  // format and colour description, which Configure() does not have (a stream
  // can hand back 10-bit one frame and 16-bit the next).
  graph_ = std::make_unique<Graph>();
  graph_->width = width;
  graph_->height = height;
  configured_transfer_ = av_transfer;
  return true;
}

bool ToneMapper::Configured() const {
  return graph_ != nullptr && !filter_graph_.empty();
}

bool ToneMapper::Matches(int width, int height, int av_transfer) const {
  return Configured() && graph_->width == width && graph_->height == height &&
         configured_transfer_ == av_transfer;
}

bool ToneMapper::BuildFilterGraph(const AVFrame& src) {
  const AVFilter* buffer = avfilter_get_by_name("buffer");
  const AVFilter* buffersink = avfilter_get_by_name("buffersink");
  if (buffer == nullptr || buffersink == nullptr) {
    return false;
  }
  AVFilterGraph* graph = avfilter_graph_alloc();
  if (graph == nullptr) {
    return false;
  }
  // sar/rate are not used by any stage here (nothing resamples spatially and
  // nothing is time-aware), but the buffer source requires them.
  char args[256];
  snprintf(args, sizeof(args),
           "video_size=%dx%d:pix_fmt=%d:time_base=1/90000:"
           "pixel_aspect=1/1:frame_rate=30/1",
           graph_->width, graph_->height, static_cast<int>(src.format));
  if (avfilter_graph_create_filter(&graph_->src, buffer, "in", args, nullptr,
                                   graph) < 0) {
    LOG(WARNING) << "tone map: buffer source refused " << graph_->width << "x"
                 << graph_->height
                 << " pix_fmt=" << static_cast<int>(src.format);
    avfilter_graph_free(&graph);
    return false;
  }
  if (avfilter_graph_create_filter(&graph_->sink, buffersink, "out", nullptr,
                                   nullptr, graph) < 0) {
    avfilter_graph_free(&graph);
    return false;
  }
  // Pin the output: the compositor's YUV->RGB matrix is BT.709 limited range,
  // so anything else would be a second, silent colour bug.
  const AVPixelFormat out_formats[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
  if (av_opt_set_int_list(graph_->sink, "pix_fmts", out_formats,
                          AV_PIX_FMT_NONE, AV_OPT_SEARCH_CHILDREN) < 0) {
    avfilter_graph_free(&graph);
    return false;
  }

  AVFilterInOut* user_in = nullptr;
  AVFilterInOut* user_out = nullptr;
  if (avfilter_graph_parse_ptr(graph, filter_graph_.c_str(), &user_in,
                               &user_out, nullptr) < 0) {
    LOG(WARNING) << "tone map: graph failed to parse: " << filter_graph_;
    avfilter_graph_free(&graph);
    return false;
  }
  bool linked = false;
  if (user_in != nullptr && user_out != nullptr) {
    linked = avfilter_link(graph_->src, 0, user_in->filter_ctx,
                           static_cast<unsigned>(user_in->pad_idx)) >= 0 &&
             avfilter_link(user_out->filter_ctx,
                           static_cast<unsigned>(user_out->pad_idx),
                           graph_->sink, 0) >= 0;
  }
  avfilter_inout_free(&user_in);
  avfilter_inout_free(&user_out);
  if (!linked) {
    avfilter_graph_free(&graph);
    return false;
  }
  if (avfilter_graph_config(graph, nullptr) < 0) {
    LOG(WARNING) << "tone map: graph configuration failed";
    avfilter_graph_free(&graph);
    return false;
  }
  graph_->graph = graph;
  return true;
}

bool ToneMapper::Process(const AVFrame& src, uint8_t* const dst_data[4],
                         const int dst_linesize[4]) {
  if (!Configured()) {
    return false;
  }
  if (graph_->graph == nullptr) {
    // First frame: the pixel format is only known now.
    if (!BuildFilterGraph(src)) {
      Reset();
      return false;
    }
  }
  AVFrame* input = av_frame_alloc();
  AVFrame* output = av_frame_alloc();
  if (input == nullptr || output == nullptr) {
    av_frame_free(&input);
    av_frame_free(&output);
    return false;
  }
  bool ok = false;
  do {
    // Shallow: the graph only reads during this call, so copying the planes
    // would cost a full frame of memory traffic for nothing.
    if (av_frame_ref(input, &src) < 0) {
      break;
    }
    input->pts = src.pts;
    if (av_buffersrc_add_frame_flags(graph_->src, input,
                                     AV_BUFFERSRC_FLAG_KEEP_REF) < 0) {
      LOG(WARNING) << "tone map: buffersrc rejected the frame";
      break;
    }
    const int ret = av_buffersink_get_frame(graph_->sink, output);
    if (ret < 0) {
      // EAGAIN here means the graph wants more input before it can emit, which
      // for a one-in-one-out chain means a misconfiguration. Reported rather
      // than looped on: an infinite loop inside a decode path is worse.
      LOG(WARNING) << "tone map: buffersink produced no frame";
      break;
    }
    // Copy plane by plane into the caller's buffers. The graph's layout and
    // the caller's can differ in stride, and dst is YUV420P by construction.
    for (int p = 0; p < 3; ++p) {
      const int rows = p == 0 ? graph_->height : (graph_->height + 1) / 2;
      const int row_bytes = p == 0 ? graph_->width : (graph_->width + 1) / 2;
      if (dst_data[p] == nullptr) {
        continue;
      }
      for (int r = 0; r < rows; ++r) {
        std::memcpy(dst_data[p] + static_cast<size_t>(r) *
                                      static_cast<size_t>(dst_linesize[p]),
                    output->data[p] +
                        static_cast<ptrdiff_t>(r) * output->linesize[p],
                    static_cast<size_t>(row_bytes));
      }
    }
    ok = true;
  } while (false);
  av_frame_free(&input);
  av_frame_free(&output);
  return ok;
}

}  // namespace xmedia::media::ffmpeg

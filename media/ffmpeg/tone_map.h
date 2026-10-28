// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// HDR -> SDR tone mapping through an FFmpeg filter graph.
//
// WHY THIS EXISTS AT ALL. The renderer has exactly one output: 8-bit SDR. An
// HDR source encodes its code values in PQ (ST 2084) or HLG (ARIB STD-B67),
// where a value is defined against an ABSOLUTE luminance up to 10000 cd/m^2.
// Handing those values to swscale and calling the result BT.709 does not
// produce a slight tint -- it throws the transfer function away, so the picture
// lands in a fraction of the display's range. Measured on the committed
// fixture (hdr_pq_320x240_1s.mp4, first frame): the naive path gives average
// luma 122, a correct Hable tonemap gives 226. Roughly half as bright, with
// highlights that never reach white.
//
// WHY A FILTER GRAPH AND NOT libzimg DIRECTLY. The obvious plan was to link
// zimg and convert. libzimg has NO tone-mapping curve -- nm on libzimg.2.dylib
// finds zero `tonemap` symbols. It converts transfer functions and resamples,
// which gives linear light and then clips everything above SDR white. The
// luminance compression lives in FFmpeg's `tonemap` filter, and that filter
// wants linear light, hence the three stages below. (An abandoned header
// written for the direct-zimg plan is kept at media/ffmpeg/tone_map.h with
// that finding recorded.)
//
// THE GRAPH:
//   zscale=t=linear:npl=<src peak>   decode PQ/HLG to linear light
//   tonemap=<curve>:desat=<k>        compress the luminance range
//   zscale=t=bt709:m=bt709:p=bt709:r=tv   encode for the SDR display
//
// WHY npl MATTERS AND IS PASSED IN: zscale needs to know what the source's
// nominal peak is to normalise before tonemapping. Guessing 10000 for an HLG
// stream whose content peaks at 1000 makes everything dimmer than it should
// be. The demuxer's mastering-display side data carries the real number when
// present; 0 here means "use the transfer function's own default".

#ifndef AVBASE_MEDIA_FFMPEG_TONE_MAP_H_
#define AVBASE_MEDIA_FFMPEG_TONE_MAP_H_

#include <memory>
#include <string>

#include "media/ffmpeg/av_includes.h"

namespace avbase::media::ffmpeg {

// Which curve compresses the luminance range. The choice is visible in the
// diagnostics line a host can print, because the three look different:
// hable is the filmic curve most HDR grades are checked against, reinhard
// keeps more mid-tone separation but can look flat, mobius preserves SDR-range
// content exactly while rolling off the top.
enum class ToneMapCurve {
  kHable = 0,
  kReinhard,
  kMobius,
};

const char* ToneMapCurveName(ToneMapCurve curve);

// True when |av_transfer| is a high-dynamic-range curve that needs mapping.
// PQ and HLG are the two in use. Anything else (BT.709, sRGB, unspecified) is
// SDR and must NOT be tone mapped: running a tonemapper over SDR content
// crushes its blacks and is the mirror image of the bug this file fixes.
bool IsHdrTransfer(int av_transfer);

// Maps HDR frames to 8-bit 4:2:0 limited-range BT.709.
//
// One instance per output geometry; Configure() rebuilds the graph when the
// geometry or the curve changes, Process() converts one frame. Not
// thread-safe, and not meant to be: it is used from the owning decoder's
// sequence, like VideoConverter next door.
class ToneMapper {
 public:
  ToneMapper();
  ~ToneMapper();
  ToneMapper(const ToneMapper&) = delete;
  ToneMapper& operator=(const ToneMapper&) = delete;

  // False when this FFmpeg build has no `tonemap`/`zscale` filter. Callers
  // must then fall back to the plain sws path AND report that they did --
  // silent degradation is what makes this class of bug look like a colour
  // cast rather than a missing feature.
  static bool IsAvailable();

  // |av_transfer| is the frame's AVFrame::color_trc (an
  // AVColorTransferCharacteristic value, taken as int to keep this header out
  // of the vendor enum). |peak_luminance| is the source's nominal peak in
  // cd/m^2; <= 0 means "let zscale use the transfer function's default".
  // |desat| is how much to desaturate out-of-gamut colours (0..1; 0.5 is a
  // reasonable default for HDR broadcast, 2.0 is the ffmpeg filter default and
  // is too strong for skin tones).
  bool Configure(int width, int height, int av_transfer, ToneMapCurve curve,
                 double peak_luminance, double desat);

  bool Configured() const;

  // Converts |src| into the caller's buffers. The frame's pixel format and
  // colour description are taken from |src| itself, so one mapper can serve a
  // stream whose frames vary.
  bool Process(const AVFrame& src, uint8_t* const dst_data[4],
               const int dst_linesize[4]);

  void Reset();

 private:
  bool BuildFilterGraph(const AVFrame& src);

  std::string filter_graph_;
  // Opaque so this header never includes libavfilter's types: the quarantine
  // is av_includes.h, and it already provides them, but keeping them out of
  // the class layout means a change there cannot recompile every caller.
  struct Graph;
  std::unique_ptr<Graph> graph_;
};

}  // namespace avbase::media::ffmpeg

#endif  // AVBASE_MEDIA_FFMPEG_TONE_MAP_H_

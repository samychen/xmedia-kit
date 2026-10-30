// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FFMPEG_FFMPEG_HW_VIDEO_DECODER_H_
#define XMEDIA_MEDIA_FFMPEG_FFMPEG_HW_VIDEO_DECODER_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/decoder_buffer.h"
#include "media/base/media_types.h"
#include "media/base/video_decoder.h"
#include "media/base/video_frame.h"
#include "media/ffmpeg/ffmpeg_video_decoder_base.h"

namespace xmedia::media::ffmpeg {

// What a platform factory asks the generic FFmpeg hw path to do. The strings
// and enums here are libav-free on purpose: the per-platform factory targets
// (platform/videotoolbox, platform/vaapi, platform/d3d11) include this header
// and they must not see a libav type (invariant C8).
struct FFmpegHwDecoderSpec {
  // FFmpeg's AVHWDeviceType name, e.g. "videotoolbox", "vaapi", "d3d11va",
  // "cuda". Resolved through av_hwdevice_iterate_types so no libav enum
  // appears in this header.
  std::string device_type;
  // Optional device node / adapter name passed to av_hwdevice_ctx_create
  // (e.g. "/dev/dri/renderD128"); empty means the FFmpeg default.
  std::string device_name;
  // The typed GPU handle the decoder's frames will carry.
  media::NativeHandleKind handle_kind{media::NativeHandleKind::kNone};
  // Codecs this device is known to decode. CreateVideoDecoder returns nullptr
  // for anything else, which is what makes the fallback chain move on.
  std::vector<media::VideoCodec> codecs;
  // Which VideoDecoderType the capability/selection layer sees (kVideoToolbox,
  // kVaapiVideoDecoder, kNvDec, ...).
  media::VideoDecoderType decoder_type{media::VideoDecoderType::kUnknown};
  std::string display_name;  // For decoder events and status details.
  // Per-device gate, evaluated against the stream's config before a decoder
  // is built. Empty means "the codec list is the only filter".
  //
  // Why a device needs one: a device can accept a stream at avcodec_open2()
  // time and reject it on the first packet -- long after Initialize() reported
  // success and the fallback chain moved on. VideoToolbox does exactly that
  // with H.264 High 4:2:2 and High 4:4:4 Predictive: "Failed setup for format
  // videotoolbox_vld: hwaccel initialisation returned error", then
  // "decode_slice_header error" on every packet, until DecoderStream abandons
  // the stream after 20 consecutive errors and the player reports
  // kDecodeFailed -- for a file the software tail decodes without complaint.
  // Declining here is the "not me" that chain already knows how to act on;
  // by the time the first packet fails, it has no candidate left to try.
  std::function<bool(const media::VideoDecoderConfig& config)> config_supported;
};

// Hardware video decoding through libavcodec's hwaccel API:
// av_hwdevice_ctx_create() for the device, avcodec_get_hw_config() + a
// get_format callback to select the hw pixel format, and decoded frames that
// stay on the GPU as VideoFrame::WrapNativeBuffer() frames (xmedia-kit §6.2).
// Pixels are read back ONLY when a consumer explicitly calls ToI420(), which
// runs av_hwframe_transfer_data behind the frame's readback callback.
//
// Fallback contract: every failure here is an Initialize/Decode status, never
// an exception and never a crash, so the existing DecoderStream chain
// (hardware first, this device's software tail last) absorbs a missing
// device, an unsupported profile or a driver reset.
//
// Decode()/Reset()/GetMaxDecodeRequests() come from
// media::FFmpegVideoDecoderBase, which holds the buffer-validation and
// callback-draining sequence shared with the software path; this class supplies
// the codec context and the hw frame transfer. It has no EOS/Reset bookkeeping
// of its own, hence no overrides for the two optional hooks.
class FFmpegHwVideoDecoder final : public media::FFmpegVideoDecoderBase {
 public:
  FFmpegHwVideoDecoder(
      FFmpegHwDecoderSpec spec,
      base::scoped_refptr<base::SequencedTaskRunner> task_runner);
  FFmpegHwVideoDecoder(const FFmpegHwVideoDecoder&) = delete;
  FFmpegHwVideoDecoder& operator=(const FFmpegHwVideoDecoder&) = delete;
  ~FFmpegHwVideoDecoder() override;

  // media::VideoDecoder:
  void Initialize(const media::VideoDecoderConfig& config, bool low_delay,
                  media::CdmContext* cdm_context, InitCB init_cb,
                  const OutputCB& output_cb,
                  const media::WaitingCB& waiting_cb) override;
  media::VideoDecoderType GetDecoderType() const override {
    return spec_.decoder_type;
  }
  const char* name() const override { return spec_.display_name.c_str(); }

 private:
  struct Context;

  bool InitializeDevice();
  bool OpenCodec(const media::VideoDecoderConfig& config);

  // media::FFmpegVideoDecoderBase:
  AVCodecContext* codec_ctx() const override;
  bool DecodeAvailableFrames() override;

  FFmpegHwDecoderSpec spec_;
  std::unique_ptr<Context> ctx_;
  media::VideoDecoderConfig config_;
};

}  // namespace xmedia::media::ffmpeg

#endif  // XMEDIA_MEDIA_FFMPEG_FFMPEG_HW_VIDEO_DECODER_H_

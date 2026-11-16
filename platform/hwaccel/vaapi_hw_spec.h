// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_PLATFORM_HWACCEL_VAAPI_HW_SPEC_H_
#define XMEDIA_PLATFORM_HWACCEL_VAAPI_HW_SPEC_H_

#include "media/base/media_types.h"
// The probe header is libav-free (only std::string), so the spec headers may
// name it -- what invariant C8 keeps out of platform/ is the libav types.
#include "media/ffmpeg/hw_device_probe.h"
#include "media/ffmpeg/ffmpeg_hw_video_decoder.h"

namespace xmedia::platform::hwaccel {

// Linux VAAPI behind libavcodec's vaapi hwaccel. Frames leave as
// VASurfaceID-backed VideoFrames; EGL import happens at the display layer,
// pixels only on an explicit ToI420().
//
// dmabuf derivation is ON here: the display half of the zero-copy path
// (platform/hwimport egl-dmabuf) can only name a plane it has an fd for, and
// a driver that refuses the derivation degrades to the explicit readback
// without changing the decoder contract. The player may still turn the whole
// derivation off per config (RenderConfig::prefer_dmabuf_zero_copy).
inline media::ffmpeg::FFmpegHwDecoderSpec VaapiHwSpec() {
  media::ffmpeg::FFmpegHwDecoderSpec spec;
  spec.device_type = "vaapi";
  // Empty device_name: FFmpeg opens the default render node. A multi-GPU
  // host that wants a specific node passes it via PlayerDeps once the
  // device-plumbing option exists.
  spec.handle_kind = media::NativeHandleKind::kVaapiSurface;
  spec.derive_dmabufs = true;
  // The host gate: a machine without a VA driver declines at the factory
  // (negotiation) instead of failing at decoder Initialize (fallback noise).
  spec.verify_build_hwaccel = true;
  spec.device_available = [type = spec.device_type, name = spec.device_name] {
    return media::ffmpeg::ProbeHwDevice(type, name);
  };
  spec.codecs = {media::VideoCodec::kH264, media::VideoCodec::kHevc,
                 media::VideoCodec::kVp9, media::VideoCodec::kAv1,
                 media::VideoCodec::kMpeg2Video};
  spec.decoder_type = media::VideoDecoderType::kVaapiVideoDecoder;
  spec.display_name = "VaapiDecoder";
  return spec;
}

}  // namespace xmedia::platform::hwaccel

#endif  // XMEDIA_PLATFORM_HWACCEL_VAAPI_HW_SPEC_H_

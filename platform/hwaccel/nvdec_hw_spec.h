// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_PLATFORM_HWACCEL_NVDEC_HW_SPEC_H_
#define XMEDIA_PLATFORM_HWACCEL_NVDEC_HW_SPEC_H_

#include <string>

#include "media/base/media_types.h"
// The probe header is libav-free (only std::string), so the spec headers may
// name it -- what invariant C8 keeps out of platform/ is the libav types.
#include "media/ffmpeg/hw_device_probe.h"
#include "media/ffmpeg/ffmpeg_hw_video_decoder.h"

namespace xmedia::platform::hwaccel {

// NVIDIA NVDEC behind libavcodec's cuda hwaccel. Frames leave as
// CUdeviceptr-backed VideoFrames (NvdecFrameInfo carries both NV12 planes and
// the decoder's CUcontext); the display side imports them through the CUDA-GL
// interop (platform/hwimport), pixels only on an explicit ToI420().
//
// The config gate below is the VideoToolbox lesson applied to NVIDIA: NVDEC
// accepts a stream at avcodec_open2() and rejects it on the first packet for
// chroma formats and profiles the hardware does not place -- High 4:4:4
// Predictive H.264 is the measured case on this machine's test corpus (the
// FFmpeg CLI decoded it for five packets and then died with I/O error).
// Declining on profile here is the "not me" the fallback chain can act on
// while it still has software candidates to try.
inline media::ffmpeg::FFmpegHwDecoderSpec NvdecHwSpec() {
  media::ffmpeg::FFmpegHwDecoderSpec spec;
  spec.device_type = "cuda";
  // Empty device_name: FFmpeg initializes the default device. A multi-GPU
  // host that wants a specific device passes it via PlayerDeps once the
  // device-plumbing option exists.
  spec.handle_kind = media::NativeHandleKind::kNvdecFrame;
  spec.codecs = {media::VideoCodec::kH264, media::VideoCodec::kHevc,
                 media::VideoCodec::kVp9};
  spec.decoder_type = media::VideoDecoderType::kNvDec;
  spec.display_name = "NvdecDecoder";
  // Same host gate as the VAAPI spec: probe once per process, decline while
  // no CUDA device is reachable.
  spec.verify_build_hwaccel = true;
  spec.device_available = [type = spec.device_type, name = spec.device_name] {
    return media::ffmpeg::ProbeHwDevice(type, name);
  };
  spec.config_supported = [](const media::VideoDecoderConfig& config) {
    // NV12 is the only layout the importer places (8-bit 4:2:0); profiles
    // that decode to 4:2:2 / 4:4:4 / 10-bit are readback-only and are
    // declined so the software tail takes them without a packet failure.
    // FFmpeg's profile names travel in config.profile as strings, so the
    // gate matches those: the measured H.264 High 4:4:4 Predictive refusal
    // and the HEVC Main 10 10-bit both land here.
    const std::string& profile = config.profile;
    if (config.codec == media::VideoCodec::kH264) {
      return profile.find("4:4:4") == std::string::npos &&
             profile.find("4:2:2") == std::string::npos &&
             profile.find("10") == std::string::npos;
    }
    if (config.codec == media::VideoCodec::kHevc) {
      return profile.find("10") == std::string::npos &&
             profile.find("4:4:4") == std::string::npos &&
             profile.find("Rext") == std::string::npos &&
             profile.find("4:2:2") == std::string::npos;
    }
    return true;  // VP9: profile 0 (8-bit) is the usual case; keep it simple.
  };
  return spec;
}

}  // namespace xmedia::platform::hwaccel

#endif  // XMEDIA_PLATFORM_HWACCEL_NVDEC_HW_SPEC_H_

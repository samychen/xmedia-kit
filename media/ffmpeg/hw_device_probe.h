// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FFMPEG_HW_DEVICE_PROBE_H_
#define XMEDIA_MEDIA_FFMPEG_HW_DEVICE_PROBE_H_

#include <string>

#include "media/base/decoder_config.h"

namespace xmedia::media::ffmpeg {

// Whether this host can actually open the hardware device |device_type| names
// ("vaapi", "cuda", ...), with |device_name| as the optional node (empty =
// FFmpeg's default). The answer is cached per (type, name) pair: the probe is
// av_hwdevice_ctx_create -- the same call the decoder's Initialize makes -- so
// a machine without the device pays for one failed attempt per PROCESS, not
// one per file, and a machine with the device pays nothing after the first.
//
// WHY THIS EXISTS. "No such hardware on this host" is capability negotiation:
// the factory that knows it must answer nullptr so the fallback chain moves
// on silently. Letting a decoder be built and fail at Initialize instead made
// every machine with the spec enabled but the device absent report a decoder
// fallback for every file -- measured on a NVIDIA-only Linux host with the
// VAAPI spec enabled, and exactly the noise the fallback counter exists to
// avoid (see PlayerDecoderEventTest.NormalPrepareReportsNoFallback).
bool ProbeHwDevice(const std::string& device_type,
                   const std::string& device_name);

// Whether THIS avcodec build can place |codec| on |device_type| at all: the
// spec's codec list is hand-written, but the hwaccels a pinned FFmpeg build
// ships are a build-time fact (setup_ffmpeg.sh enables exactly one family --
// a VAAPI spec on an nvdec-only build accepted the stream at the factory and
// failed at OpenCodec with "unsupported-codec", which the fallback counter
// reported as noise). Asking avcodec_get_hw_config is free and authoritative.
// An unknown codec name answers true: the software tail's own resolution
// failure is the honest error for that case, not a factory decline.
bool BuildHasHwCodecForDevice(const std::string& codec_name,
                              media::VideoCodec codec,
                              const std::string& device_type);

// Resolves an AVHWDeviceType from FFmpeg's own device-type list by iteration,
// so no libav enum is named by the callers that hold only the spec's string.
// Shared with the hw decoder (which uses it at Initialize).
int DeviceTypeFromName(const std::string& name);

}  // namespace xmedia::media::ffmpeg

#endif  // XMEDIA_MEDIA_FFMPEG_HW_DEVICE_PROBE_H_

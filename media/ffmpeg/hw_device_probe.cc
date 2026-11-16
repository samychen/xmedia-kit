// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/hw_device_probe.h"

#include <mutex>
#include <unordered_map>

#include "base/logging.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"

namespace xmedia::media::ffmpeg {
namespace {

// 0 = not probed yet, 1 = device opens, 2 = device absent/unusable.
constexpr int kUnknown = 0;
constexpr int kAvailable = 1;
constexpr int kAbsent = 2;

int ProbeResultFor(const std::string& key, AVHWDeviceType type,
                   const std::string& device_name) {
  static std::mutex cache_mutex;
  static std::unordered_map<std::string, int> cache;
  std::lock_guard<std::mutex> lock(cache_mutex);
  auto it = cache.find(key);
  if (it != cache.end() && it->second != kUnknown) {
    return it->second;
  }
  AVBufferRef* device_ref = nullptr;
  const int ret = av_hwdevice_ctx_create(
      &device_ref, type, device_name.empty() ? nullptr : device_name.c_str(),
      nullptr, 0);
  if (ret < 0) {
    if (device_ref) {
      av_buffer_unref(&device_ref);
    }
    cache[key] = kAbsent;
    LOG(INFO) << "hw device probe: '" << key << "' cannot open on this host ("
              << AvErrorString(ret) << "); hardware specs will decline";
    return kAbsent;
  }
  av_buffer_unref(&device_ref);
  cache[key] = kAvailable;
  return kAvailable;
}

}  // namespace

bool ProbeHwDevice(const std::string& device_type,
                   const std::string& device_name) {
  const AVHWDeviceType type =
      static_cast<AVHWDeviceType>(DeviceTypeFromName(device_type));
  if (type == AV_HWDEVICE_TYPE_NONE) {
    // An unknown device type is not "absent hardware", it is a spec typo --
    // answer available and let the decoder's Initialize report the real
    // error, which names the type it could not resolve.
    return true;
  }
  const int result =
      ProbeResultFor(device_type + "|" + device_name, type, device_name);
  return result != kAbsent;
}

bool BuildHasHwCodecForDevice(const std::string& codec_name,
                              media::VideoCodec codec,
                              const std::string& device_type) {
  // The stream's codec name (the demuxer's spelling, e.g. "h264") resolves
  // directly; without one, fall back to the config codec's canonical name.
  const AVCodec* av_codec = nullptr;
  if (!codec_name.empty()) {
    av_codec = avcodec_find_decoder_by_name(codec_name.c_str());
  }
  if (!av_codec) {
    switch (codec) {
    case media::VideoCodec::kH264:
      av_codec = avcodec_find_decoder(AV_CODEC_ID_H264);
      break;
    case media::VideoCodec::kHevc:
      av_codec = avcodec_find_decoder(AV_CODEC_ID_HEVC);
      break;
    case media::VideoCodec::kVp9:
      av_codec = avcodec_find_decoder(AV_CODEC_ID_VP9);
      break;
    case media::VideoCodec::kMpeg2Video:
      av_codec = avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
      break;
    default:
      break;
    }
  }
  if (!av_codec) {
    return true;  // benefit of the doubt: let the decoder report the truth
  }
  const int type_int = DeviceTypeFromName(device_type);
  if (type_int == static_cast<int>(AV_HWDEVICE_TYPE_NONE)) {
    // FFmpeg's device-type list IS the build configuration: a type it does
    // not enumerate was not compiled in, so no hw_config can place this
    // codec on it. Measured: "vaapi" on the nvdec-only pinned build lands
    // here, and the "spec typo" reading would have let the decoder be built
    // and fail at OpenCodec -- the noise this gate exists to remove.
    return false;
  }
  const AVHWDeviceType wanted = static_cast<AVHWDeviceType>(type_int);
  for (int i = 0;; ++i) {
    const AVCodecHWConfig* hw_config = avcodec_get_hw_config(av_codec, i);
    if (!hw_config) {
      return false;  // no config places this codec on this device type
    }
    if ((hw_config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) &&
        hw_config->device_type == wanted) {
      return true;
    }
  }
}

int DeviceTypeFromName(const std::string& name) {
  AVHWDeviceType type = AV_HWDEVICE_TYPE_NONE;
  while ((type = av_hwdevice_iterate_types(type)) != AV_HWDEVICE_TYPE_NONE) {
    const char* type_name = av_hwdevice_get_type_name(type);
    if (type_name && name == type_name) {
      return static_cast<int>(type);
    }
  }
  return static_cast<int>(AV_HWDEVICE_TYPE_NONE);
}

}  // namespace xmedia::media::ffmpeg

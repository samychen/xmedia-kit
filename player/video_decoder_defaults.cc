// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/video_decoder_defaults.h"

#include "xmedia-kit/BuildConfig.h"
#include "base/memory/scoped_refptr.h"
#include "media/ffmpeg/ffmpeg_hw_decoder_factory.h"
#if XMEDIA_ENABLE_D3D11
#include "platform/hwaccel/d3d11_hw_spec.h"
#endif
#if XMEDIA_ENABLE_VAAPI
#include "platform/hwaccel/vaapi_hw_spec.h"
#endif
#if XMEDIA_ENABLE_NVDEC
#include "platform/hwaccel/nvdec_hw_spec.h"
#endif
#if XMEDIA_ENABLE_VIDEOTOOLBOX
#include "platform/hwaccel/videotoolbox_hw_spec.h"
#endif

namespace xmedia {

std::vector<base::scoped_refptr<media::VideoDecoderFactory>>
DefaultHardwareVideoDecoderFactories(
    const base::scoped_refptr<base::SequencedTaskRunner>& video_runner,
    media::HwCodecMask allowed_codecs, bool prefer_dmabuf_zero_copy) {
  std::vector<base::scoped_refptr<media::VideoDecoderFactory>> out;
  // One factory class, three specs. The #if order is irrelevant: at most one
  // spec compiles on any given host platform.
#if XMEDIA_ENABLE_VIDEOTOOLBOX
  using media::ffmpeg::FFmpegHwVideoDecoderFactory;
  out.emplace_back(base::MakeRefCounted<FFmpegHwVideoDecoderFactory>(
      platform::hwaccel::VideotoolboxHwSpec(), video_runner, allowed_codecs));
#endif
#if XMEDIA_ENABLE_VAAPI
  {
    using media::ffmpeg::FFmpegHwVideoDecoderFactory;
    auto spec = platform::hwaccel::VaapiHwSpec();
    // The one config knob on this spec: without it the derivation would run
    // for hosts that asked not to pay for it, and the field would stay the
    // declared-but-unread shape the option discipline forbids.
    spec.derive_dmabufs = prefer_dmabuf_zero_copy;
    out.push_back(base::MakeRefCounted<FFmpegHwVideoDecoderFactory>(
        std::move(spec), video_runner, allowed_codecs));
  }
#endif
#if XMEDIA_ENABLE_NVDEC
  // NVIDIA: after the VAAPI backend (a machine with both resolves by order,
  // and NVDEC on a VA-equipped host is harmless -- the config gate declines
  // what the VA device would have taken first anyway).
  {
    out.push_back(base::MakeRefCounted<media::ffmpeg::FFmpegHwVideoDecoderFactory>(
        platform::hwaccel::NvdecHwSpec(), video_runner, allowed_codecs));
  }
#endif
#if XMEDIA_ENABLE_D3D11
  using media::ffmpeg::FFmpegHwVideoDecoderFactory;
  out.push_back(base::MakeRefCounted<FFmpegHwVideoDecoderFactory>(
      platform::hwaccel::D3D11HwSpec(), video_runner, allowed_codecs));
#endif
  return out;
}

}  // namespace xmedia

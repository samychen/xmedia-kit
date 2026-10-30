// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// AVStream -> xmedia-kit decoder config, split out of ffmpeg_demuxer.cc (C1).
//
// This is the seam the demuxer's own length note names. The three functions
// here are pure: an AVStream in, one of our configs out, no access to the
// AVFormatContext, its packet queues or its lifetime. What is left in
// ffmpeg_demuxer.cc is one contiguous AVFormatContext lifecycle, and cutting
// pieces of THAT by line count is how use-after-free bugs get introduced.
//
// Internal to xmedia_ffmpeg; not installed.

#ifndef XMEDIA_MEDIA_FFMPEG_DEMUXER_CONFIGS_H_
#define XMEDIA_MEDIA_FFMPEG_DEMUXER_CONFIGS_H_

#include "media/base/decoder_config.h"

struct AVStream;

namespace xmedia::media {

VideoDecoderConfig MakeVideoDecoderConfig(const AVStream* stream);
AudioDecoderConfig MakeAudioDecoderConfig(const AVStream* stream);
TextDecoderConfig MakeTextDecoderConfig(const AVStream* stream);

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_FFMPEG_DEMUXER_CONFIGS_H_

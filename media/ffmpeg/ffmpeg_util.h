// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Type bridges between FFmpeg's enums and avbase's. Every mapping that more
// than one file under media/ffmpeg/ needs lives here, exactly once.
//
// Why this file and not compat.h: compat.h absorbs FFmpeg *version*
// differences (the `#if LIBAV*` blocks, which C8 keeps in one place). These
// tables are version-stable, so they were not a compat problem -- they were a
// copy-paste problem. Each table below had been written out once per consumer:
// the VideoFormat -> AVPixelFormat table existed twice (video_convert.cc and
// ffmpeg_video_decoder.cc), the AVSampleFormat -> SampleFormat table twice
// (ffmpeg_audio_decoder.cc and, inline, ffmpeg_demuxer_configs.cc), and the
// AVFormatContext cast twice (ffmpeg_demuxer.cc and
// ffmpeg_demuxer_seek.cc). Two copies of one table eventually disagree with
// each other; the two AVSampleFormat copies already did -- one defaulted to
// kUnknown, the other kept the previously negotiated format -- and only a
// side-by-side read shows they are otherwise identical.
//
// Naming follows compat.h's ToAv*/FromAv* convention. The |fallback| argument
// is what the unmapped cases return: it lets one table serve both "unknown is
// an error" (probe: kUnknown) and "keep what we had" (decode: current format).

#ifndef AVBASE_MEDIA_FFMPEG_FFMPEG_UTIL_H_
#define AVBASE_MEDIA_FFMPEG_FFMPEG_UTIL_H_

#include "media/base/audio_parameters.h"
#include "media/base/video_frame.h"
#include "media/ffmpeg/av_includes.h"

namespace avbase::media::ffmpeg {

// ---- Pixel formats ---------------------------------------------------------
// AV_PIX_FMT_NONE for the formats avbase has no equivalent for (kYUY2,
// kYUV420P10): the caller must treat that as "cannot express" and fail or
// convert, never as a valid destination. YV12 deliberately maps to
// AV_PIX_FMT_YUV420P: avbase's YV12 is the same plane set with V and U
// swapped, which only matters to code that reads planes positionally.
[[nodiscard]] AVPixelFormat ToAvPixelFormat(media::VideoFormat format);

// |fallback| is returned for every unmapped format, including
// AV_PIX_FMT_NONE. 422/444 planar collapse to kI420, which is not a lossless
// mapping but is the right answer for a render path: VideoConverter is the
// component that owns "this needs conversion", and it decides how.
[[nodiscard]] media::VideoFormat FromAvPixelFormat(AVPixelFormat format,
                                                   media::VideoFormat fallback);

// ---- Sample formats --------------------------------------------------------
// AV_SAMPLE_FMT_NONE for kUnknown: an unset container sample format must not
// be silently guessed into a real one.
[[nodiscard]] AVSampleFormat ToAvSampleFormat(media::SampleFormat format);

// |fallback| is returned for every unmapped format, including
// AV_SAMPLE_FMT_NONE (what a container that declares nothing reports).
[[nodiscard]] media::SampleFormat
FromAvSampleFormat(AVSampleFormat format, media::SampleFormat fallback);

// ---- Opaque demuxer handles ------------------------------------------------
// FFmpegDemuxer carries its AVFormatContext across threads as a void* (the
// demux thread owns it, the control thread only nulls the handle), so the
// cast appears at each hand-off. Keeping it here means the "who may touch the
// context" rule is written once instead of once per file.
[[nodiscard]] AVFormatContext* AsFormatContext(void* raw);

}  // namespace avbase::media::ffmpeg

#endif  // AVBASE_MEDIA_FFMPEG_FFMPEG_UTIL_H_

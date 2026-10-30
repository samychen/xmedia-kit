// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/transcode/ffmpeg_preview_job.h"

#include "base/logging.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"
#include "media/transcode/ffmpeg_transcode_streams.h"

namespace xmedia::media {
namespace {

namespace ff = ::xmedia::media::ffmpeg;

}  // namespace

Status PreviewFrames(const std::string& input_uri, double start_time_seconds,
                     int max_frames,
                     std::vector<base::scoped_refptr<VideoFrame>>* out,
                     const TranscodeCancelToken* cancel) {
  if (!out || max_frames <= 0) {
    return base::unexpected(
        MediaError(ErrorCode::kInvalidArgument, "preview: nothing to grab",
                   max_frames <= 0 ? "max_frames <= 0" : "out is null",
                   "pass a positive frame count"));
  }
  out->clear();

  ff::FormatCtxPtr in(nullptr);
  AVFormatContext* raw_in = nullptr;
  if (avformat_open_input(&raw_in, input_uri.c_str(), nullptr, nullptr) < 0) {
    return base::unexpected(MediaError(
        ErrorCode::kSourceOpenFailed, "preview: cannot open " + input_uri,
        "avformat_open_input failed",
        "check the source exists and is a supported container"));
  }
  in.reset(raw_in);
  if (avformat_find_stream_info(in.get(), nullptr) < 0) {
    return base::unexpected(MediaError(ErrorCode::kSourceReadFailed,
                                       "preview: cannot probe " + input_uri,
                                       "avformat_find_stream_info failed", {}));
  }

  int video_index = -1;
  for (unsigned i = 0; i < in->nb_streams; ++i) {
    if (in->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      video_index = static_cast<int>(i);
      break;
    }
  }
  if (video_index < 0) {
    return base::unexpected(
        MediaError(ErrorCode::kSourceUnsupported,
                   "preview: the source has no video "
                   "stream",
                   "nb_streams=" + std::to_string(in->nb_streams),
                   "previews need a video track"));
  }
  AVStream* stream = in->streams[video_index];

  const AVCodec* dec = avcodec_find_decoder(stream->codecpar->codec_id);
  if (!dec) {
    return base::unexpected(
        MediaError(ErrorCode::kSourceUnsupported,
                   "preview: no decoder for the video "
                   "codec",
                   "codec_id=" + std::to_string(stream->codecpar->codec_id),
                   "re-encode the source with a codec this build decodes"));
  }
  ff::CodecCtxPtr decoder(avcodec_alloc_context3(dec));
  if (avcodec_parameters_to_context(decoder.get(), stream->codecpar) < 0) {
    return base::unexpected(MediaError(ErrorCode::kSourceReadFailed,
                                       "preview: decoder parameter copy failed",
                                       {}, {}));
  }
  decoder->pkt_timebase = stream->time_base;
  if (avcodec_open2(decoder.get(), dec, nullptr) < 0) {
    return base::unexpected(MediaError(
        ErrorCode::kSourceReadFailed, "preview: avcodec_open2 failed", {}, {}));
  }

  if (start_time_seconds > 0.0) {
    const int64_t target =
        static_cast<int64_t>(start_time_seconds * AV_TIME_BASE);
    if (av_seek_frame(in.get(), -1, target, AVSEEK_FLAG_BACKWARD) < 0) {
      LOG(WARNING) << "preview: seek to " << start_time_seconds
                   << "s failed; grabbing from start";
    } else {
      // After a seek the decoder must be drained of the pre-seek frames
      // still sitting in its reorder buffer, or frame one of the preview is
      // from before the seek point.
      avcodec_flush_buffers(decoder.get());
    }
  }

  // av_seek_frame lands on the keyframe at or before the target, which for a
  // 2-second-GOP source means asking for 0.5 s and getting 0 s. For a single
  // snapshot that is fine (and is what ffmpeg's -ss does); for a scrubber it
  // is not -- every preview in the first GOP is the same picture. So decode
  // forward and discard anything before the target. The keyframe is still
  // where decoding starts (that is unavoidable with inter-frame codecs);
  // what changes is which frames are KEPT.
  const int64_t keep_from_us =
      start_time_seconds > 0.0
          ? static_cast<int64_t>(start_time_seconds * AV_TIME_BASE)
          : INT64_MIN;

  ff::PacketPtr packet(av_packet_alloc());
  ff::FramePtr frame(av_frame_alloc());
  while (static_cast<int>(out->size()) < max_frames) {
    if (cancel && cancel->IsCancelled()) {
      return base::unexpected(MediaError(
          ErrorCode::kCancelled, "preview cancelled", {},
          "partial results are not returned: the caller asked for a window, "
          "not a stream"));
    }
    if (av_read_frame(in.get(), packet.get()) < 0) {
      break;  // EOF: deliver what was collected.
    }
    if (packet->stream_index != video_index) {
      av_packet_unref(packet.get());
      continue;
    }
    if (avcodec_send_packet(decoder.get(), packet.get()) < 0) {
      av_packet_unref(packet.get());
      continue;  // Skip-on-error: one bad packet never kills a preview.
    }
    av_packet_unref(packet.get());
    while (avcodec_receive_frame(decoder.get(), frame.get()) >= 0) {
      const int64_t frame_us =
          frame->pts == AV_NOPTS_VALUE
              ? keep_from_us
              : av_rescale_q(frame->pts, decoder->pkt_timebase,
                             AVRational{1, AV_TIME_BASE});
      // Dropped frames are the cost of the keyframe start; they are not
      // errors and must not be counted as "decoded nothing".
      if (frame_us < keep_from_us) {
        av_frame_unref(frame.get());
        continue;
      }
      out->push_back(AvFrameToVideoFrame(frame.get(), frame->width,
                                         frame->height, decoder->pkt_timebase));
      av_frame_unref(frame.get());
      if (static_cast<int>(out->size()) >= max_frames) {
        break;
      }
    }
  }
  if (out->empty()) {
    return base::unexpected(
        MediaError(ErrorCode::kSourceReadFailed, "preview: no frames decoded",
                   "seek target " + std::to_string(start_time_seconds) +
                       "s, max " + std::to_string(max_frames),
                   "the seek target may be past the last keyframe"));
  }
  return Status();
}

}  // namespace xmedia::media

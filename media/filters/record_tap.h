// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FILTERS_RECORD_TAP_H_
#define XMEDIA_MEDIA_FILTERS_RECORD_TAP_H_

#include <functional>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "media/base/decoder_buffer.h"
#include "media/media_export.h"

namespace xmedia::media {

// The playback-side recording tap: the packets a viewer actually sees, as
// they leave the demuxer stream and enter the decoder stream -- after the
// seek-generation filter (a seek redirects the record), after track
// selection (a switch changes the record), before decode (copy stays copy).
//
// Pure media types only: the recorder that consumes these lives behind the
// opt-in transcode target and turns them into container packets.
struct XMEDIA_MEDIA_EXPORT RecordStreamHeader {
  bool video = false;
  std::string codec_name;  // Decode-side name ("aac", "h264", ...).
  int width = 0;
  int height = 0;
  int sample_rate = 0;
  int channels = 0;
  // Codec private data (h264 AVCC, AAC AudioSpecificConfig) — a muxer needs
  // it to build the output track; without it an h264 copy is undecodable.
  std::vector<uint8_t> extra_data;
};

// One tap serves one player: on_stream fires once per decoder open (a track
// switch opens a new decoder and re-fires), on_packet for every buffer that
// reached the decoder -- end-of-stream buffers included, which is how the
// recorder learns the presentation ended. Delivered on the renderer's own
// sequence (S3/S4); the recorder marshals.
// Pipeline-level tap: the video flag travels with the packet, because one
// recorder serves both legs.
struct XMEDIA_MEDIA_EXPORT RecordTap {
  std::function<void(const RecordStreamHeader&)> on_stream;
  std::function<void(bool video, base::scoped_refptr<DecoderBuffer> buffer)>
      on_packet;
};

// Decoder-stream-level tap: the stream knows its own kind, so its packet
// callback carries no flag. The renderer binds the flag when applying a
// pipeline-level tap (BindTap below).
struct XMEDIA_MEDIA_EXPORT StreamRecordTap {
  std::function<void(const RecordStreamHeader&)> on_stream;
  std::function<void(base::scoped_refptr<DecoderBuffer>)> on_packet;
};

// Binds the video flag into a stream-level tap.
inline StreamRecordTap BindTap(const RecordTap& tap, bool video) {
  StreamRecordTap stream_tap;
  stream_tap.on_stream = tap.on_stream;
  stream_tap.on_packet = [video, cb = tap.on_packet](
                             base::scoped_refptr<DecoderBuffer> buffer) {
    cb(video, std::move(buffer));
  };
  return stream_tap;
}

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_FILTERS_RECORD_TAP_H_

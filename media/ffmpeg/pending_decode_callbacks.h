// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FFMPEG_PENDING_DECODE_CALLBACKS_H_
#define AVBASE_MEDIA_FFMPEG_PENDING_DECODE_CALLBACKS_H_

#include <deque>
#include <utility>

#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "media/base/video_decoder.h"

namespace avbase::media {

// Owns the queue of outstanding Decode() callbacks for an FFmpeg video decoder
// and posts them (never inline) on the decoder's sequence. Software and
// hardware decoders share this so the "post, never run inline" contract and
// the EOS/Reset drain loops live in exactly one place instead of being copied
// across FFmpegVideoDecoder and FFmpegHwVideoDecoder.
//
// The decoder keeps its own |task_runner_| for unrelated posts (init_cb, the
// Reset closure); this class holds an independent ref to the same runner.
class PendingDecodeCallbacks {
 public:
  explicit PendingDecodeCallbacks(
      base::scoped_refptr<base::SequencedTaskRunner> task_runner)
      : task_runner_(std::move(task_runner)) {}

  // Enqueue one Decode() callback. Mirrors deque::push_back.
  void Push(VideoDecoder::DecodeCB cb) { pending_.push_back(std::move(cb)); }

  // Post and pop exactly one callback. No-op if the queue is empty (e.g. a
  // Reset already drained it). Used for the single-buffer decode path.
  void RunOne(DecoderStatus status);

  // Post and pop every outstanding callback. Used at EOS (|status| ok) and on
  // Reset/error (|status| carries the abort reason).
  void RunAll(DecoderStatus status);

 private:
  base::scoped_refptr<base::SequencedTaskRunner> task_runner_;
  std::deque<VideoDecoder::DecodeCB> pending_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FFMPEG_PENDING_DECODE_CALLBACKS_H_

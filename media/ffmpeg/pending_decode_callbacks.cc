// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/pending_decode_callbacks.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"

namespace avbase::media {

void PendingDecodeCallbacks::RunOne(DecoderStatus status) {
  if (pending_.empty()) {
    return;
  }
  VideoDecoder::DecodeCB cb = std::move(pending_.front());
  pending_.pop_front();
  // Posted, never run inline: the VideoDecoder contract requires it, and
  // DecoderStream relies on it to avoid re-entrant Decode() calls.
  task_runner_->PostTask(FROM_HERE,
                         base::BindOnce(std::move(cb), std::move(status)));
}

void PendingDecodeCallbacks::RunAll(DecoderStatus status) {
  while (!pending_.empty()) {
    VideoDecoder::DecodeCB cb = std::move(pending_.front());
    pending_.pop_front();
    task_runner_->PostTask(FROM_HERE, base::BindOnce(std::move(cb), status));
  }
}

}  // namespace avbase::media

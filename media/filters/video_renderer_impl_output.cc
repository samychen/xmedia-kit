// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The sink-facing half of VideoRendererImpl: the two methods the sink calls
// back into, which run on the sink's render sequence (S6) rather than on S3.
//
// Split out of video_renderer_impl.cc to stay under the 500-line ratchet (C1),
// not because it is a separate concern. It is the only part of this class that
// runs off S3, and that is exactly why every task it creates is bound to the
// owner's WeakPtr: a post from S6 races the destructor on S3 by construction.

#include "media/filters/video_renderer_impl.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"

namespace xmedia::media {

base::scoped_refptr<VideoFrame>
VideoRendererImpl::Render(base::TimeTicks deadline_min,
                          base::TimeTicks deadline_max) {
  // The whole body is one locked call into the compositor. Nothing here may
  // block, allocate or call back into Player: this runs on the sink's render
  // sequence, which for the SDL2 backend is the window's event loop (docs/04
  // §1, S6) -- stalling it freezes input handling as well as video.
  const size_t pending_before = compositor_.frames_pending();
  base::scoped_refptr<VideoFrame> frame =
      compositor_.Render(deadline_min, deadline_max);
  // Feed the video clock from here, on S6: this is the one sequence that knows
  // a frame actually left for the display, which mirrors ffplay setting
  // vidclk inside video_refresh(). A null frame means "repeat the previous
  // one", and repeating must not move the clock.
  if (frame && frame_presented_cb_) {
    frame_presented_cb_.Run(frame->timestamp(), frame->serial());
  }
  // Consumer wake-up for the decode pump (counterpart of ffplay's
  // frame_queue_signal): the pump stops at compositor back-pressure, so it must
  // be re-armed whenever the compositor *freed room* -- a present OR a drop --
  // not only on a present. The original `if (frame)` alone let the pump starve
  // when the compositor drained by dropping frames (e.g. the fps cap): a drop
  // returns null and never re-armed PumpDecoder, so buffered decoder output
  // was never drained, the decoder stream jammed at its watermark, and a file
  // that played to its end never reached kCompleted. A hold/repeat leaves the
  // pending count unchanged, so it correctly does NOT re-arm (this is what
  // distinguishes the fix from the "re-arm whenever not full" variant, which
  // over-reads and drops a frame the test expects to see presented).
  // Posted to S3; PumpDecoder re-checks every condition there, so a spurious
  // kick is a cheap no-op.
  //
  // Weak-bound, not Unretained: this post is issued from S6, the destructor
  // runs on S3, and nothing orders the two. Unretained here meant a queued
  // kick could run PumpDecoder against a freed object -- the .ips reports
  // showed the abort inside the compositor's lock, which is the second member
  // PumpDecoder touched.
  if (compositor_.frames_pending() < pending_before) {
    task_runner_->PostTask(FROM_HERE,
                           base::BindOnce(&VideoRendererImpl::PumpDecoder,
                                          weak_factory_.GetWeakPtr()));
  }
  return frame;
}

void VideoRendererImpl::OnFrameSubmitFailure() {
  // Counted rather than retried: a present failure is almost always a surface
  // that went away (resize, minimise, compositor restart), and the next
  // Render() will be asked for the right frame anyway. Retrying here would
  // present a frame whose deadline has already passed. base/logging.h has no
  // LOG_EVERY_N (the first draft used it), so the rate-limiting is a plain
  // counter. It must be rate-limited somehow: this runs on the render sequence
  // at display rate, so an unthrottled LOG(WARNING) is ~60 lines/second for as
  // long as the surface is broken.
  const uint64_t n = submit_failures_.fetch_add(1) + 1;
  if (n == 1 || n % 100 == 0) {
    LOG(WARNING) << "xmedia.vout: frame submit failed (" << n << " times)";
  }
}

}  // namespace xmedia::media

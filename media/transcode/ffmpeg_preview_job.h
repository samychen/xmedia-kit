// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_TRANSCODE_PREVIEW_JOB_H_
#define XMEDIA_MEDIA_TRANSCODE_PREVIEW_JOB_H_

#include <cstdint>
#include <string>
#include <vector>

#include "base/memory/scoped_refptr.h"
#include "media/base/media_error.h"
#include "media/base/video_frame.h"
#include "media/media_export.h"
#include "media/transcode/ffmpeg_transcode_job.h"  // TranscodeCancelToken

namespace xmedia::media {

// Seek-preview frame grab (the batch half of the thumbnail capability:
// get-frame-mode + TakeSnapshot cover the single-frame case). Seeks |input|
// to |start_time_seconds| (keyframe seek, ffmpeg.c's -ss semantics) and
// decodes up to |max_frames| video frames from that point on. No encode, no
// mux — the frames come back as xmedia-kit VideoFrames for the host to turn
// into
// a filmstrip, a scrubber preview, or whatever the product needs.
//
// Synchronous and blocking, like Transcode(); cancellable at packet
// boundaries through the same token type.
Status XMEDIA_MEDIA_EXPORT
PreviewFrames(const std::string& input_uri, double start_time_seconds,
              int max_frames, std::vector<base::scoped_refptr<VideoFrame>>* out,
              const TranscodeCancelToken* cancel = nullptr);

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_TRANSCODE_PREVIEW_JOB_H_

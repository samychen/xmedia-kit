// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FFMPEG_IMAGE_SNAPSHOT_H_
#define XMEDIA_MEDIA_FFMPEG_IMAGE_SNAPSHOT_H_

#include <string>

#include "base/memory/scoped_refptr.h"
#include "media/base/media_error.h"
#include "media/media_export.h"

namespace xmedia::media {

class VideoFrame;

// Encodes |frame| (I420/NV12/RGB planar or packed -- converted through sws to
// the encoder's input format) into a single JPEG and writes it to |path|.
// This is TakeSnapshot's backend: one frame, one file, no container. Returns
// a MediaError with the actionable suggestion on every failure mode
// (encoder missing from the FFmpeg build, file not writable, frame not
// mappable).
Status XMEDIA_MEDIA_EXPORT WriteJpegSnapshot(const VideoFrame& frame,
                                             const std::string& path);

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_FFMPEG_IMAGE_SNAPSHOT_H_

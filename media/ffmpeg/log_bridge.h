// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FFMPEG_LOG_BRIDGE_H_
#define XMEDIA_MEDIA_FFMPEG_LOG_BRIDGE_H_

#include <string>

#include "base/logging.h"
#include "media/ffmpeg/av_includes.h"

namespace xmedia::media::ffmpeg {

// Routes av_log() into base/logging, so a host that installed a
// base::logging::LoggingDelegate sees FFmpeg's messages alongside xmedia-kit's.
//
// ijkplayer hard-codes __android_log_print in ijksdl_android_jni.c and writes
// to stderr everywhere else; this is the platform-independent replacement
// (behaviour difference Δ22).
//
// Install once from FFmpegGlue::InitializeFFmpeg(). Idempotent.
void InstallLogBridge();

// Maps an AV_LOG_* level onto xmedia-kit's severities. Exposed for testing.
base::logging::LogSeverity MapAvLogLevel(int av_level);

// GetFFmpegVersionString() is declared in ffmpeg_glue.h, which is the header
// that exports it; it is not part of the log bridge.

}  // namespace xmedia::media::ffmpeg

#endif  // XMEDIA_MEDIA_FFMPEG_LOG_BRIDGE_H_

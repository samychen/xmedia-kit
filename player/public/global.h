// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_PLAYER_PUBLIC_GLOBAL_H_
#define XMEDIA_PLAYER_PUBLIC_GLOBAL_H_

#include "player/public/player_export.h"

namespace xmedia {

// One-time, idempotent process-wide initialisation: bridges the vendor log,
// initialises FFmpeg exactly once, and calls XInitThreads() when the process
// will use X11.
//
// Every Player constructor calls this, so you normally never need to. Call it
// explicitly only when you must guarantee it happens before your own first
// X11 call (see docs/archive/09 §10 L1).
//
// Thread safety: safe to call concurrently; guarded by std::call_once.
XMEDIA_PLAYER_EXPORT void GlobalInit();

// Version of xmedia-kit, e.g. "0.1.0".
XMEDIA_PLAYER_EXPORT const char* GetVersion();

// Version of the FFmpeg xmedia-kit was built against, e.g. "7.1", or "none"
// when
// built with XMEDIA_ENABLE_FFMPEG=OFF.
XMEDIA_PLAYER_EXPORT const char* GetFFmpegVersion();

// RAII helper for programs that want symmetric init/shutdown.
class XMEDIA_PLAYER_EXPORT GlobalGuard {
 public:
  GlobalGuard() { GlobalInit(); }
  GlobalGuard(const GlobalGuard&) = delete;
  GlobalGuard& operator=(const GlobalGuard&) = delete;
  ~GlobalGuard() = default;
};

}  // namespace xmedia

#endif  // XMEDIA_PLAYER_PUBLIC_GLOBAL_H_

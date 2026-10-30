// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `base/threading/platform_thread.h` (BSD-3-Clause).

#ifndef XMEDIA_BASE_THREADING_PLATFORM_THREAD_H_
#define XMEDIA_BASE_THREADING_PLATFORM_THREAD_H_

#include <string>

#include "base/base_export.h"

namespace xmedia::base {

enum class ThreadPriority {
  kBackground,
  kBestEffort,
  kDisplayCritical,  // Audio and video render threads.
};

namespace platform_thread {

// Sets the OS-visible name of the calling thread. Linux truncates to 15
// characters, so xmedia-kit names stay short: "xmedia-media", "xmedia-demux",
// "xmedia-video", "xmedia-audio", "xmedia-event".
XMEDIA_BASE_EXPORT void SetName(const std::string& name);
XMEDIA_BASE_EXPORT std::string GetName();
XMEDIA_BASE_EXPORT void SetCurrentThreadPriority(ThreadPriority priority);

}  // namespace platform_thread
}  // namespace xmedia::base

#endif  // XMEDIA_BASE_THREADING_PLATFORM_THREAD_H_

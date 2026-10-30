// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Forwarding header: the type lives in media/base so that sinks can use it,
// but SDK users include it from here to keep the public surface in one place.

#ifndef XMEDIA_PLAYER_PUBLIC_NATIVE_DISPLAY_H_
#define XMEDIA_PLAYER_PUBLIC_NATIVE_DISPLAY_H_

#include "media/base/native_display.h"

namespace xmedia {
using media::NativeDisplay;
using media::NativeDisplayKind;
using media::WaylandSurfaceHandle;
using media::X11WindowHandle;
}  // namespace xmedia

#endif  // XMEDIA_PLAYER_PUBLIC_NATIVE_DISPLAY_H_

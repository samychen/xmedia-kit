// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Shared internals of the C ABI (docs/08 M15) -- NOT a public header.
//
// The C ABI is split across three .cc files (player / display / events) to stay
// inside the C1 500-line limit. What they share lives here: the opaque handle
// definitions, the enum translations, and the string copy-out helper.
//
// The handles are declared at GLOBAL scope, not in an anonymous namespace: the
// public header's typedefs already bind these names, and re-declaring them
// inside a namespace would make every unqualified use ambiguous.

#ifndef XMEDIA_PLAYER_PUBLIC_C_XMEDIA_C_INTERNAL_H_
#define XMEDIA_PLAYER_PUBLIC_C_XMEDIA_C_INTERNAL_H_

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "base/functional/bind.h"
#include "base/time/time.h"
#include "media/base/media_error.h"
#include "media/base/media_types.h"
#include "media/base/native_display.h"
#include "media/base/video_frame.h"
#include "player/public/c/xmedia_c.h"

#include "player/public/c/xmedia_c.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/time/time.h"
#include "media/base/media_error.h"
#include "media/base/media_info.h"
#include "media/base/media_types.h"
#include "media/base/native_display.h"
#include "media/base/video_frame.h"
#include "player/public/global.h"
#include "player/public/media_info.h"
#include "player/public/player.h"
#include "player/public/player_event.h"
#include "player/public/scene.h"

// Defined at GLOBAL scope, not in an anonymous namespace: the header's typedefs
// already bind these names in C, and re-declaring them inside a namespace makes
// every unqualified use ambiguous.
struct XmediaPlayer {
  std::unique_ptr<xmedia::Player> player;
};

struct XmediaNativeDisplay {
  xmedia::base::scoped_refptr<xmedia::media::NativeDisplay> display;
};

struct XmediaSource {
  std::shared_ptr<xmedia::SceneSource> source;
};

namespace {

// ---------------------------------------------------------------------------
// Enum translation
// ---------------------------------------------------------------------------

// The C codes are a frozen list declared in the same order as the C++ enum. A
// plain static_cast would compile and be silently wrong if someone reordered
// the header, so the mapping goes through a switch and an unnamed code becomes
// a loud generic failure instead of an arbitrary number.
// inline on every function defined in this header, and not as a style choice:
// the three .cc files each use a DIFFERENT subset of these helpers, so a TU
// that does not call one sees an unused static function and warns. It is also
// what makes the definitions legal here at all -- a non-inline definition in a
// header included by three translation units is a duplicate symbol. Neither
// showed up before, because XMEDIA_ENABLE_CAPI was OFF in every preset, so
// these three files had never been compiled by any gate.
inline XmediaStatus ToC(xmedia::ErrorCode code) {
  switch (code) {
  case xmedia::ErrorCode::kOk:
    return XMEDIA_OK;
  case xmedia::ErrorCode::kInvalidArgument:
    return XMEDIA_ERR_INVALID_ARGUMENT;
  case xmedia::ErrorCode::kInvalidState:
    return XMEDIA_ERR_INVALID_STATE;
  case xmedia::ErrorCode::kNotImplemented:
    return XMEDIA_ERR_NOT_IMPLEMENTED;
  case xmedia::ErrorCode::kOutOfMemory:
    return XMEDIA_ERR_OUT_OF_MEMORY;
  case xmedia::ErrorCode::kAborted:
    return XMEDIA_ERR_ABORTED;
  case xmedia::ErrorCode::kTimeout:
    return XMEDIA_ERR_TIMEOUT;
  case xmedia::ErrorCode::kSourceOpenFailed:
    return XMEDIA_ERR_SOURCE_OPEN_FAILED;
  case xmedia::ErrorCode::kSourceNotFound:
    return XMEDIA_ERR_SOURCE_NOT_FOUND;
  case xmedia::ErrorCode::kSourceReadFailed:
    return XMEDIA_ERR_SOURCE_READ_FAILED;
  case xmedia::ErrorCode::kDecoderNotFound:
    return XMEDIA_ERR_DECODER_NOT_FOUND;
  case xmedia::ErrorCode::kDecoderOpenFailed:
    return XMEDIA_ERR_DECODER_OPEN_FAILED;
  case xmedia::ErrorCode::kDecodeFailed:
    return XMEDIA_ERR_DECODE_FAILED;
  case xmedia::ErrorCode::kDecoderUnsupportedCodec:
    return XMEDIA_ERR_DECODER_UNSUPPORTED_CODEC;
  case xmedia::ErrorCode::kSinkNotAttached:
    return XMEDIA_ERR_SINK_NOT_ATTACHED;
  case xmedia::ErrorCode::kStreamNotFound:
    return XMEDIA_ERR_STREAM_NOT_FOUND;
  case xmedia::ErrorCode::kTrackNotFound:
    return XMEDIA_ERR_TRACK_NOT_FOUND;
  case xmedia::ErrorCode::kConfigInvalid:
    return XMEDIA_ERR_CONFIG_INVALID;
  case xmedia::ErrorCode::kEos:
  case xmedia::ErrorCode::kSourceEos:
    return XMEDIA_ERR_EOS;
  default:
    // Codes the ABI does not name yet still have to produce something a C
    // host can branch on, and "failed" is the only honest answer.
    return XMEDIA_ERR_FAILED;
  }
}

inline XmediaStatus ToC(const xmedia::Status& status) {
  // Status is base::expected<void, MediaError>. A success carries no error, so
  // the error() call is guarded here rather than left to convention.
  if (status.has_value()) {
    return XMEDIA_OK;
  }
  return ToC(status.error().code());
}

inline int64_t ToMs(xmedia::base::TimeDelta delta) {
  return delta.InMilliseconds();
}

inline xmedia::media::DemuxerStreamType FromStreamType(int stream_type) {
  switch (stream_type) {
  case XMEDIA_STREAM_AUDIO:
    return xmedia::media::DemuxerStreamType::kAudio;
  case XMEDIA_STREAM_VIDEO:
    return xmedia::media::DemuxerStreamType::kVideo;
  case XMEDIA_STREAM_TEXT:
    return xmedia::media::DemuxerStreamType::kText;
  default:
    return xmedia::media::DemuxerStreamType::kUnknown;
  }
}

inline xmedia::media::NativeDisplayKind FromDisplayKind(int kind) {
  switch (kind) {
  case XMEDIA_DISPLAY_KIND_X11_WINDOW:
    return xmedia::media::NativeDisplayKind::kX11Window;
  case XMEDIA_DISPLAY_KIND_WAYLAND_SURFACE:
    return xmedia::media::NativeDisplayKind::kWaylandSurface;
  case XMEDIA_DISPLAY_KIND_SDL2_WINDOW:
    return xmedia::media::NativeDisplayKind::kSdl2Window;
  case XMEDIA_DISPLAY_KIND_DRM_MASTER:
    return xmedia::media::NativeDisplayKind::kDrmMaster;
  case XMEDIA_DISPLAY_KIND_GBM_DEVICE:
    return xmedia::media::NativeDisplayKind::kGbmDevice;
  case XMEDIA_DISPLAY_KIND_ANDROID_NATIVE_WINDOW:
    return xmedia::media::NativeDisplayKind::kAndroidNativeWindow;
  case XMEDIA_DISPLAY_KIND_ANDROID_SURFACE:
    return xmedia::media::NativeDisplayKind::kAndroidSurface;
  case XMEDIA_DISPLAY_KIND_CAMETAL_LAYER:
    return xmedia::media::NativeDisplayKind::kCametalLayer;
  case XMEDIA_DISPLAY_KIND_CAEAGL_LAYER:
    return xmedia::media::NativeDisplayKind::kCaEaglLayer;
  case XMEDIA_DISPLAY_KIND_HWND:
    return xmedia::media::NativeDisplayKind::kHwnd;
  case XMEDIA_DISPLAY_KIND_GLFW_WINDOW:
    return xmedia::media::NativeDisplayKind::kGlfwWindow;
  case XMEDIA_DISPLAY_KIND_EGL_WINDOW:
    return xmedia::media::NativeDisplayKind::kEglWindow;
  default:
    return xmedia::media::NativeDisplayKind::kNone;
  }
}

// Copies |text| into the caller's buffer and returns the full length excluding
// the NUL, so a caller can size a buffer and retry. Writing past the
// capacity is
// the classic C ABI crash; truncating silently is the classic C ABI lie. This
// does both halves correctly and never allocates.
inline int32_t CopyOut(const std::string& text, char* out, int32_t capacity) {
  const int32_t needed = static_cast<int32_t>(text.size());
  if (out == nullptr || capacity <= 0) {
    return needed;
  }
  const int32_t writable = std::min(capacity - 1, needed);
  if (writable > 0) {
    std::memcpy(out, text.data(), static_cast<size_t>(writable));
  }
  out[writable] = '\0';
  return needed;
}

// Holds the C function pointer and its user_data as a pair. A shared_ptr
// because the Player may outlive the C handle during destruction, and a raw
// capture would dangle exactly when a late event can arrive.
struct EventBridge {
  XmediaEventCallback callback{nullptr};
  void* user_data{nullptr};
};

// None() is a VALID non-rendering target, so validity is not "non-null" --
// but a null pointer here still means the factory refused (bad handle), and
// both are a failure to create.
inline XmediaNativeDisplay*
WrapDisplay(xmedia::base::scoped_refptr<xmedia::media::NativeDisplay> display) {
  if (display == nullptr || !display->valid()) {
    return nullptr;
  }
  std::unique_ptr<XmediaNativeDisplay> handle(new (std::nothrow)
                                                  XmediaNativeDisplay());
  if (handle == nullptr) {
    return nullptr;
  }
  handle->display = std::move(display);
  return handle.release();
}

}  // namespace

#endif  // XMEDIA_PLAYER_PUBLIC_C_XMEDIA_C_INTERNAL_H_

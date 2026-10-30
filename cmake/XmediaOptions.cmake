# Copyright 2026 The xmedia-kit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

option(XMEDIA_ENABLE_FFMPEG       "Build the FFmpeg demuxer/decoder adapters" OFF)
# The libcurl HTTP(S) data source (media/curl). Opt-in like FFmpeg: a host
# that is happy with the avio-backed UrlDataSource never pays for libcurl.
option(XMEDIA_ENABLE_CURL         "Build the libcurl HTTP(S) data source"     OFF)
option(XMEDIA_ENABLE_SDL2         "Build the SDL2 video/audio backend"        OFF)
option(XMEDIA_ENABLE_QT           "Build the Qt GL backend (needs Qt 6.4+)"  OFF)
option(XMEDIA_ENABLE_LINUX_NATIVE "Build the native Linux GL/X11/Wayland backend" OFF)
option(XMEDIA_ENABLE_ANDROID      "Build the Android backend"                 OFF)
option(XMEDIA_ENABLE_IOS          "Build the iOS backend"                     OFF)
option(XMEDIA_ENABLE_CAPI         "Build the C ABI compatibility layer"       OFF)
option(XMEDIA_BUILD_SHARED        "Build shared libraries instead of static"  OFF)

# Hardware decoder factories (Phase 3). Each is a thin FFmpeg-hwaccel factory:
# the decode work is the platform's video device, the frames leave as typed
# NativeHandles, and the fallback chain absorbs a missing device. A factory
# only builds when XMEDIA_ENABLE_FFMPEG does (they instantiate
# FFmpegHwVideoDecoder); platform/CMakeLists enforces that.
if(APPLE)
  option(XMEDIA_ENABLE_VIDEOTOOLBOX "Build the VideoToolbox hw decoder factory" ON)
else()
  option(XMEDIA_ENABLE_VIDEOTOOLBOX "Build the VideoToolbox hw decoder factory" OFF)
endif()
option(XMEDIA_ENABLE_VAAPI  "Build the VAAPI hw decoder factory"   OFF)
option(XMEDIA_ENABLE_D3D11  "Build the D3D11 hw decoder factory"   OFF)

option(XMEDIA_BUILD_TESTS    "Build unit/contract/integration tests" ${XMEDIA_IS_TOP_LEVEL})
option(XMEDIA_BUILD_EXAMPLES "Build example programs"               ${XMEDIA_IS_TOP_LEVEL})
option(XMEDIA_BUILD_QT_PLAYER "Build the Qt Quick player/editor app (needs Qt 6.4+)" OFF)
# The reusable Qt Quick facade. Separate from (and implied by) the app: a host
# that wants PlayerBridge/VideoItem/GlVideoItem without the demo app asks for
# this and nothing else. XMEDIA_ENABLE_QT is the *GL backend* under it and not
# a substitute -- the backend is inert without something to draw, and the
# facade cannot build without the backend.
option(XMEDIA_BUILD_QT_FACADE "Build the reusable Qt Quick facade library (player/qt/, needs Qt 6.4+)" OFF)
if(XMEDIA_BUILD_QT_PLAYER AND NOT XMEDIA_BUILD_QT_FACADE)
  message(STATUS
      "XMEDIA_BUILD_QT_PLAYER=ON implies XMEDIA_BUILD_QT_FACADE=ON "
      "(the app links the facade; it does not own those sources any more)")
  set(XMEDIA_BUILD_QT_FACADE ON CACHE BOOL
      "Build the reusable Qt Quick facade library (player/qt/, needs Qt 6.4+)"
      FORCE)
endif()
option(XMEDIA_BUILD_BENCH    "Build benchmarks"                     OFF)
option(XMEDIA_BUILD_FUZZ     "Build libFuzzer targets"              OFF)
option(XMEDIA_INSTALL        "Generate install rules"               ${XMEDIA_IS_TOP_LEVEL})

option(XMEDIA_STRICT_WARNINGS "Enable -Wconversion / -Wold-style-cast" OFF)
option(XMEDIA_WERROR          "Treat warnings as errors"               OFF)
option(XMEDIA_ENABLE_LTO      "Enable IPO/LTO for release builds"      OFF)
option(XMEDIA_COVERAGE        "Enable gcov instrumentation"            OFF)
option(XMEDIA_ENABLE_DCHECK   "Enable DCHECK / SEQUENCE_CHECKER"       ON)

set(XMEDIA_SANITIZERS "" CACHE STRING "Semicolon list: address;thread;undefined")
set(XMEDIA_FFMPEG_ROOT "" CACHE PATH "Prefix where FFmpeg is installed")

if("address" IN_LIST XMEDIA_SANITIZERS AND "thread" IN_LIST XMEDIA_SANITIZERS)
  message(FATAL_ERROR "ASan and TSan cannot be enabled together")
endif()
if(XMEDIA_ENABLE_ANDROID AND XMEDIA_ENABLE_IOS)
  message(FATAL_ERROR "Android and iOS backends are mutually exclusive")
endif()

# NOTE: XMEDIA_BUILD_TESTS is deliberately *not* gated on XMEDIA_ENABLE_FFMPEG.
# Building and testing base/ + media/ + player/ without FFmpeg installed is the
# executable proof of design goal G2; see docs/06 §2.

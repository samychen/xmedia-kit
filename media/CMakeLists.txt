# media/ — media framework. media/base holds interfaces and value types,
# media/filters holds the portable implementations, media/ffmpeg holds
# everything that touches libav*, and media/transcode holds the offline
# transcode product line. See docs/02 §8 and decision D11.
#
# Source listing (docs/06 §7.6): base/, filters/, renderers/ and
# filters/legacy/ are all globbed, because in each of them directory
# membership equals target membership -- every .cc there belongs to
# xmedia_media, and none is selected per platform or per build option, so a
# new file needs no edit here.
#
# filters/ used to be the exception: it held the ffmpeg_*.cc files next to the
# core filters, and because those belong to xmedia_ffmpeg a glob would have
# swept libav* into the core target (G2), so it stayed a hand-written list.
# Both directories are single-target now that all the FFmpeg code has one home.
# Invariant C25 fails the gate when a .cc lands in no target at all, so
# "forgot to list it" cannot stay silent.
file(GLOB XMEDIA_MEDIA_BASE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/base/*.cc")
file(GLOB XMEDIA_MEDIA_FILTER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/filters/*.cc")
file(GLOB XMEDIA_MEDIA_LEGACY_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/filters/legacy/*.cc")
file(GLOB XMEDIA_MEDIA_RENDERER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/renderers/*.cc")
# media/gl is the video-domain half of the GL path: gl_present (YUV planes,
# colour matrices) and gl_video_sink (the render thread and the
# VideoRendererSink contract). Its GL primitives come from xmedia_gl, which
# knows nothing about frames -- the split is what lets render/gl stand alone.
file(GLOB XMEDIA_MEDIA_GL_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/gl/*.cc")

add_library(xmedia_media
    # The Pipeline interfaces in this glob were promoted from DRAFT in the tenth
    # round. Each is the key function for its class's vtable, which is why their
    # constructors and destructors are out-of-line (see media/base/demuxer.cc).
    ${XMEDIA_MEDIA_BASE_SOURCES}
    ${XMEDIA_MEDIA_FILTER_SOURCES}
    ${XMEDIA_MEDIA_RENDERER_SOURCES}
    ${XMEDIA_MEDIA_GL_SOURCES}
    # LGPL-2.1 quarantine: line-by-line ports of ff_ffplay.c algorithms.
    # See media/filters/legacy/README.md and root LICENSE section 1.
    ${XMEDIA_MEDIA_LEGACY_SOURCES})
add_library(xmedia::media ALIAS xmedia_media)

target_include_directories(xmedia_media PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/xmedia-kit>)
target_link_libraries(xmedia_media PUBLIC xmedia::base xmedia::gl)
target_compile_definitions(xmedia_media PRIVATE XMEDIA_MEDIA_IMPLEMENTATION=1)
xmedia_configure_target(xmedia_media)

# FFmpeg adapter layer. This is the ONLY target in the project that links the
# FFmpeg:: libraries; xmedia_media and xmedia_base never see a libav* header
# (design goal G2, invariants C4/C8, docs/02 §8). xmedia_transcode, below,
# builds on this target and links no vendor library of its own.
#
# It sits under media/ and not platform/ because FFmpeg is not a platform. It
# is a portable framework that behaves identically on every host, whereas the
# platform layer (sdl2, hwaccel) holds code that is CHOSEN by the host. What
# earns this a separate target is the dependency; what earns it a separate
# directory is that a directory must equal a target for the globs above to be
# safe (docs/06 §7.6).
if(XMEDIA_ENABLE_FFMPEG)
  # ffmpeg/ qualifies for a glob: every .cc in it belongs to this one target and
  # none is selected per platform (av_includes.h absorbs version differences).
  file(GLOB XMEDIA_FFMPEG_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/*.cc")
  add_library(xmedia_ffmpeg ${XMEDIA_FFMPEG_SOURCES})
  add_library(xmedia::ffmpeg ALIAS xmedia_ffmpeg)

  target_include_directories(xmedia_ffmpeg PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
  target_link_libraries(xmedia_ffmpeg
      PUBLIC  xmedia::media
      PRIVATE FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil
              FFmpeg::swscale FFmpeg::swresample FFmpeg::avfilter)
  target_compile_definitions(xmedia_ffmpeg PRIVATE XMEDIA_ENABLE_FFMPEG=1)
  # Optional SIMD fast paths for video_convert.cc; when absent the module
  # compiles to a sws_scale-only fallback and nothing else changes.
  find_package(Libyuv QUIET)
  if(Libyuv_FOUND)
    target_link_libraries(xmedia_ffmpeg PRIVATE libyuv::libyuv)
    target_compile_definitions(xmedia_ffmpeg PUBLIC XMEDIA_HAVE_LIBYUV=1)
  endif()
  xmedia_configure_target(xmedia_ffmpeg)
  # One precompiled header for the whole vendor layer: av_includes.h is the
  # single extern "C" wrap point, so every TU here needs it anyway.
  target_precompile_headers(xmedia_ffmpeg PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/av_includes.h")

  # Offline transcode: the Phase 5 product line (encoders, mux writer, the
  # Transcode/Concat jobs, remux). It is FFmpeg code, so it is gated by the same
  # option and sits on top of xmedia_ffmpeg -- but it is a separate TARGET
  # because nothing in the playback path uses it: the ten .cc here had zero
  # production consumers outside media/ffmpeg when this split was made (the one
  # caller that looked like one -- player's TakeSnapshot -- goes through
  # media/ffmpeg/ffmpeg_image_snapshot instead). Keeping it in xmedia_ffmpeg
  # meant every playback build carried the encoder and muxer code; as its own
  # target a transcode consumer asks for xmedia::transcode explicitly, and the
  # playback SDK stops paying for it. It is deliberately NOT part of the
  # xmedia::xmedia-kit umbrella for the same reason.
  file(GLOB XMEDIA_TRANSCODE_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/transcode/*.cc")
  add_library(xmedia_transcode ${XMEDIA_TRANSCODE_SOURCES})
  add_library(xmedia::transcode ALIAS xmedia_transcode)
  target_include_directories(xmedia_transcode PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
  # PUBLIC, not PRIVATE: transcode's public headers name media types, and its
  # .cc files reach into the av_includes.h / compat.h / video_convert.h glue.
  # The FFmpeg::* libraries themselves stay PRIVATE to xmedia_ffmpeg, so this
  # target still links no vendor library of its own.
  target_link_libraries(xmedia_transcode PUBLIC xmedia::ffmpeg)
  # The transcode .cc files name libav* headers, exactly as the media/ffmpeg/
  # ones do -- include isolation was given up on purpose (D11 chose link
  # isolation plus directory isolation). What must not happen is a SECOND
  # target linking the FFmpeg:: libraries, which is why this asks for the
  # include directories rather than for FFmpeg::avcodec. Same shape as
  # media_ffmpeg_unittests, media_transcode_unittests and xmedia-inspect, which
  # add these for themselves: xmedia_ffmpeg links FFmpeg PRIVATE, so they are
  # not inherited.
  target_include_directories(xmedia_transcode SYSTEM PRIVATE
      ${FFmpeg_INCLUDE_DIRS})
  xmedia_configure_target(xmedia_transcode)
endif()

# media/curl — the libcurl-backed HTTP(S) data source, the port of
# CicadaPlayer's curl_data_source (pinned DNS, forced IPv4/6, custom headers,
# proxy, low-speed watchdog). Its own target because it links CURL::libcurl:
# xmedia_media must stay vendor-free (G2), and the dependency earns the
# target, the target earns the directory (docs/06 §7.6) -- the same shape as
# xmedia_ffmpeg above. Nothing in media/ or player/ links it implicitly; the
# player facade does so when XMEDIA_ENABLE_CURL is on.
if(XMEDIA_ENABLE_CURL)
  find_package(CURL REQUIRED)
  file(GLOB XMEDIA_CURL_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/curl/*.cc")
  add_library(xmedia_curl ${XMEDIA_CURL_SOURCES})
  add_library(xmedia::curl ALIAS xmedia_curl)
  target_include_directories(xmedia_curl PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
  target_link_libraries(xmedia_curl
      PUBLIC  xmedia::media
      PRIVATE CURL::libcurl)
  target_compile_definitions(xmedia_curl PRIVATE XMEDIA_CURL_IMPLEMENTATION=1)
  xmedia_configure_target(xmedia_curl)
endif()

# --- CEF OSR：HTML 组合图层（HyperFrames 对齐，2026-11 移植） -------------
# 单独 target：CEF 是重依赖（二进制 distro + wrapper 构建），且产物是
# GlLayerSource 的一个生产者——播放 SDK 不默认携带。门控语义对齐
# MediaComponent：distro 缺失绝不 fail configure，降级 XMEDIA_HAS_CEF=0。
if(XMEDIA_ENABLE_CEF)
  set(XMEDIA_CEF_ROOT "" CACHE PATH "Path to the CEF binary distro")
  if(XMEDIA_CEF_ROOT STREQUAL "")
    foreach(_candidate "${CMAKE_SOURCE_DIR}/../cef"
                        "$ENV{HOME}/Develop/vspo/cef")
      if(EXISTS "${_candidate}/include/cef_version.h")
        set(XMEDIA_CEF_ROOT "${_candidate}")
        break()
      endif()
    endforeach()
  endif()
  set(XMEDIA_HAS_CEF 0 CACHE INTERNAL "")
  if(APPLE AND EXISTS "${XMEDIA_CEF_ROOT}/include/cef_version.h"
             AND EXISTS "${XMEDIA_CEF_ROOT}/Release/Chromium Embedded Framework.framework")
    set(XMEDIA_HAS_CEF 1 CACHE INTERNAL "")
  elseif(UNIX AND NOT APPLE AND EXISTS "${XMEDIA_CEF_ROOT}/include/cef_version.h"
                              AND EXISTS "${XMEDIA_CEF_ROOT}/Release/libcef.so")
    set(XMEDIA_HAS_CEF 1 CACHE INTERNAL "")
  endif()
  if(NOT XMEDIA_HAS_CEF)
    message(WARNING "XMEDIA_ENABLE_CEF=ON but no usable CEF distro at "
                    "${XMEDIA_CEF_ROOT}; HTML layer disabled")
    set(XMEDIA_ENABLE_CEF OFF CACHE BOOL "" FORCE)
  endif()
endif()

if(XMEDIA_HAS_CEF)
  message(STATUS "XMEDIA_ENABLE_CEF: CEF distro at ${XMEDIA_CEF_ROOT}")
  set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${XMEDIA_CEF_ROOT}/cmake")
  set(_X_SAVE_OSX_ARCHS "${CMAKE_OSX_ARCHITECTURES}")
  set(_X_SAVE_OSX_DEPLOY "${CMAKE_OSX_DEPLOYMENT_TARGET}")
  if(POLICY CMP0074)
    cmake_policy(SET CMP0074 NEW)
  endif()
  set(CEF_ROOT "${XMEDIA_CEF_ROOT}")
  find_package(CEF)
  if(NOT DEFINED CEF_LIBCEF_DLL_WRAPPER_PATH
     OR NOT IS_DIRECTORY "${CEF_LIBCEF_DLL_WRAPPER_PATH}")
    message(WARNING "find_package(CEF) failed at ${XMEDIA_CEF_ROOT}; "
                    "HTML layer disabled")
    set(XMEDIA_HAS_CEF 0 CACHE INTERNAL "")
    set(XMEDIA_ENABLE_CEF OFF CACHE BOOL "" FORCE)
    set(CMAKE_OSX_ARCHITECTURES "${_X_SAVE_OSX_ARCHS}")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${_X_SAVE_OSX_DEPLOY}")
  else()
    add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)
    set(CMAKE_OSX_ARCHITECTURES "${_X_SAVE_OSX_ARCHS}")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "${_X_SAVE_OSX_DEPLOY}")

    file(GLOB XMEDIA_CEF_SOURCES CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/cef/*.cc"
        "${CMAKE_CURRENT_SOURCE_DIR}/cef/*.cpp")
    add_library(xmedia_cef ${XMEDIA_CEF_SOURCES})
    add_library(xmedia::cef ALIAS xmedia_cef)
    target_include_directories(xmedia_cef PUBLIC
        $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
        ${CEF_INCLUDE_PATH})
    target_compile_definitions(xmedia_cef PUBLIC XMEDIA_HAS_CEF=1)
    # CEF 头要求 C++20；仅对 cef 源提升（其余 target 保持各自标准）。
    set_source_files_properties(${XMEDIA_CEF_SOURCES} PROPERTIES
        COMPILE_FLAGS "-std=c++20")
    target_link_libraries(xmedia_cef
        PUBLIC  xmedia::media libcef_dll_wrapper
        PRIVATE ${CEF_STANDARD_LIBS})
    add_dependencies(xmedia_cef libcef_dll_wrapper)
    # Subprocess 入口：独立小可执行（Chromium 以 --type=... 重启它）。
    # macOS 也必须链 framework：子进程在加载期就要解析 cef_* 符号（dyld
    # 两级命名空间不做 dynamic_lookup——未解析即 crash，GPU 子进程
    # exit 11 实测）。CEF 156 的 macOS distro 不设 CEF_LIB_RELEASE
    # （那是 libcef.so/Linux 的变量），直接用 framework 二进制路径；
    # BUILD_RPATH 指向 distro Release，让 helper 拷贝到任何位置都能解析
    # （LC_RPATH 是绝对路径，随文件复制保留）。
    if(APPLE)
      set(XMEDIA_CEF_FRAMEWORK_LIB
          "${XMEDIA_CEF_ROOT}/Release/Chromium Embedded Framework.framework/Chromium Embedded Framework")
      if(NOT EXISTS "${XMEDIA_CEF_FRAMEWORK_LIB}")
        message(WARNING "CEF framework binary missing at ${XMEDIA_CEF_FRAMEWORK_LIB}")
        set(XMEDIA_HAS_CEF 0)
      endif()
    else()
      ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
      set(XMEDIA_CEF_FRAMEWORK_LIB libcef_lib)
    endif()
    add_executable(xmedia_cef_helper cef/cef_helper_main.cc cef/cef_app.cpp)
    target_compile_definitions(xmedia_cef_helper PRIVATE XMEDIA_HAS_CEF=1)
    target_include_directories(xmedia_cef_helper PRIVATE
        ${CMAKE_SOURCE_DIR} ${CEF_INCLUDE_PATH})
    target_link_libraries(xmedia_cef_helper PRIVATE libcef_dll_wrapper
        "${XMEDIA_CEF_FRAMEWORK_LIB}" ${CEF_STANDARD_LIBS})
    if(APPLE)
      set_target_properties(xmedia_cef_helper PROPERTIES
          BUILD_RPATH "${XMEDIA_CEF_ROOT}/Release")
    endif()
    add_dependencies(xmedia_cef_helper libcef_dll_wrapper)
    if(UNIX AND NOT APPLE)
      ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")
      target_link_libraries(xmedia_cef PUBLIC libcef_lib)
    endif()
    xmedia_configure_target(xmedia_cef)
  endif()
endif()



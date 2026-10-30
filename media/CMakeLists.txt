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

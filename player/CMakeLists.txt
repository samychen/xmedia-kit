# player/ — the SDK facade. Only player/public/*.h is installed.
#
# Source listing (docs/06 §7.6): one directory, one target, nothing selected per
# platform or per build option -- so the list is a glob and a new file needs no
# edit here. Invariant C25 catches a .cc that lands in no target at all.
file(GLOB XMEDIA_PLAYER_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
add_library(xmedia_player ${XMEDIA_PLAYER_SOURCES})
add_library(xmedia::player ALIAS xmedia_player)
target_include_directories(xmedia_player PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/xmedia-kit>)
target_link_libraries(xmedia_player PUBLIC xmedia::media)
# The facade assembles the default decoder-factory list (player_impl.cc),
# including the FFmpeg-hwaccel hardware factory whose specs live in
# platform/{videotoolbox,vaapi,d3d11} (header-only; the factory class itself
# is in xmedia_ffmpeg).
if(XMEDIA_ENABLE_FFMPEG)
  target_link_libraries(xmedia_player PUBLIC xmedia::ffmpeg)
endif()
# The curl transport is opt-in exactly like FFmpeg: WrapRetry() instantiates
# media::curl::CurlDataSource only when the build has it AND the config asks
# for it (NetConfig::prefer_curl), so the default avio path never changes.
if(XMEDIA_ENABLE_CURL)
  target_link_libraries(xmedia_player PUBLIC xmedia::curl)
endif()
target_compile_definitions(xmedia_player PRIVATE XMEDIA_PLAYER_IMPLEMENTATION=1)
xmedia_configure_target(xmedia_player)

add_library(xmedia-kit INTERFACE)
add_library(xmedia::xmedia-kit ALIAS xmedia-kit)
target_link_libraries(xmedia-kit INTERFACE xmedia::player)
if(XMEDIA_ENABLE_FFMPEG)
  target_link_libraries(xmedia-kit INTERFACE xmedia::ffmpeg)
endif()
if(XMEDIA_ENABLE_CURL)
  target_link_libraries(xmedia-kit INTERFACE xmedia::curl)
endif()
if(XMEDIA_ENABLE_SDL2)
  target_link_libraries(xmedia-kit INTERFACE xmedia::platform_sdl2)
endif()

# player/qt/ — the Qt Quick facade. A subdirectory rather than a sibling of
# player/ because it is part of the SDK's host surface (apps/ is for consumers
# and nothing under base/ media/ player/ render/ platform/ may include it),
# and its own target because the whole directory is what one option selects
# (docs/06 §7.6: one directory, one target).
if(XMEDIA_BUILD_QT_FACADE)
  add_subdirectory(qt)
endif()

# Opt-in transcode entry on the Player API surface. NOT part of xmedia_player
# (playback builds must not pay for encoders; media/CMakeLists decision D11):
# a host that transcodes links this target explicitly, next to xmedia::transcode.
#
# Gated on FFmpeg for the same reason xmedia::transcode is: this target's only
# reason to exist is to forward to it, and its public header includes
# transcode/ffmpeg_transcode_job.h. Without the gate, a no-ffmpeg configure
# fails outright ("links to xmedia::transcode, but the target was not found")
# -- which is exactly what happened, because the target was added
# unconditionally while the library it needs is conditional.
if(XMEDIA_ENABLE_FFMPEG)
  file(GLOB XMEDIA_PLAYER_TRANSCODE_SOURCES CONFIGURE_DEPENDS
      "${CMAKE_CURRENT_SOURCE_DIR}/player_transcode.cc")
  add_library(xmedia_player_transcode ${XMEDIA_PLAYER_TRANSCODE_SOURCES})
  add_library(xmedia::player_transcode ALIAS xmedia_player_transcode)
  target_include_directories(xmedia_player_transcode PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
      $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>)
  target_link_libraries(xmedia_player_transcode
      PUBLIC xmedia::player xmedia::transcode)
  xmedia_configure_target(xmedia_player_transcode)
endif()

# Install / export / packaging rules (docs/06 §9 "M13: XmediaInstall.cmake +
# xmedia.map"). Included from the top-level CMakeLists.txt ONLY when this is
# the top-level project: a tree consumed via add_subdirectory() must not
# install anything into the consumer's prefix as a side effect of building.
#
# WHAT IS EXPORTED AND WHY. The core four libraries (base, media, gl, player)
# plus xmedia_ffmpeg enter the xmedia-kitTargets export set:
#
#   * base/media/gl/player are the portable core -- a host links them with
#     FFmpeg and nothing else (V3, tools/check_link_deps.sh enforces it).
#   * xmedia_ffmpeg joins the set because xmedia_player links it PUBLIC when
#     XMEDIA_ENABLE_FFMPEG is on, and CMake refuses to write an export file
#     whose targets name a non-exported one. Its FFmpeg::* INTERFACE IMPORTED
#     dependencies are exported as find_dependency(FFmpeg) calls, which is
#     why FindFFmpeg.cmake ships INSIDE the package below -- CMake has no
#     built-in FindFFmpeg, so a consumer without our module would fail to
#     satisfy that dependency.
#   * xmedia_transcode / xmedia_platform_* / xmedia_cabi stay install-only:
#     nothing in the export set links them, and an exported optional backend
#     turns a dlopen-loadable choice into a hard find_package requirement.
#     They remain usable via explicit add_library(IMPORTED), which is what
#     the "integration by hand" path in docs/10 §12 V4 does anyway.
#
# HEADERS are installed as the whole tree rather than a computed closure of
# player/public: the facade headers include media/base and base/ headers by
# design (MediaError, NativeDisplay, PlaybackStats all live there -- invariant
# C22 puts shared types in media/base), so the closure is "most of the tree"
# with no stable edge. Copying the tree cannot miss a header; computing the
# closure can, and the failure is a consumer build error.

if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  return()
endif()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# ---- libraries -------------------------------------------------------------
set(XMEDIA_EXPORT_LIBS xmedia_base xmedia_media xmedia_gl xmedia_player)
if(TARGET xmedia_ffmpeg)
  list(APPEND XMEDIA_EXPORT_LIBS xmedia_ffmpeg)
endif()
# Same reason xmedia_ffmpeg joins: xmedia_player links it PUBLIC when
# XMEDIA_ENABLE_CURL is on, so the export set must name it. CURL needs no
# module shipped along -- CMake has had a built-in FindCURL providing
# CURL::libcurl since 3.17, which is above this project's 3.20 floor.
if(TARGET xmedia_curl)
  list(APPEND XMEDIA_EXPORT_LIBS xmedia_curl)
endif()
# EXPORT_NAME makes the installed surface identical to the in-tree aliases:
# a consumer writes xmedia::player, exactly what the examples and the docs
# use. Without it the export would be xmedia::xmedia_player -- the target's
# file name, which no document has ever promised. Set one by one: a single
# set_target_properties call with five EXPORT_NAME keys does NOT pair them up
# (CMake keeps the last value for every target), it warns and exports one
# name five times.
foreach(pair "xmedia_base;base" "xmedia_media;media" "xmedia_gl;gl"
              "xmedia_player;player" "xmedia_ffmpeg;ffmpeg"
              "xmedia_curl;curl")
  list(GET pair 0 _target)
  list(GET pair 1 _export_name)
  if(TARGET ${_target})
    set_target_properties(${_target} PROPERTIES EXPORT_NAME ${_export_name})
  endif()
endforeach()
install(TARGETS ${XMEDIA_EXPORT_LIBS}
    EXPORT xmedia-kitTargets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# Optional layers: installed but NOT exported (see the header comment).
set(XMEDIA_INSTALL_ONLY_LIBS)
foreach(lib xmedia_transcode xmedia_platform_sdl2
             xmedia_platform_glfw xmedia_platform_egl xmedia_cabi)
  if(TARGET ${lib})
    list(APPEND XMEDIA_INSTALL_ONLY_LIBS ${lib})
  endif()
endforeach()
if(XMEDIA_INSTALL_ONLY_LIBS)
  install(TARGETS ${XMEDIA_INSTALL_ONLY_LIBS}
      ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
      LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
      INCLUDES DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
endif()

# ---- headers ---------------------------------------------------------------
# NO trailing slash on the directory names: install(DIRECTORY foo/) installs
# foo's CONTENTS into the destination, which turned player/ into
# include/public/ and dropped the layer every #include in the tree spells
# ("player/public/player.h"). Without the slash each directory installs as a
# directory, so the installed tree keeps the source layout exactly -- the
# include paths a host uses are then the same strings the examples use.
install(DIRECTORY
      "${CMAKE_CURRENT_SOURCE_DIR}/base"
      "${CMAKE_CURRENT_SOURCE_DIR}/media"
      "${CMAKE_CURRENT_SOURCE_DIR}/render"
      "${CMAKE_CURRENT_SOURCE_DIR}/player"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
    FILES_MATCHING PATTERN "*.h")
# The generated pair (BuildConfig.h / Version.h) is what a host uses to
# feature-test the build it installed; it lands at include/xmedia-kit/.
install(DIRECTORY "${CMAKE_BINARY_DIR}/generated/xmedia-kit/"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/xmedia-kit"
    FILES_MATCHING PATTERN "*.h")

# ---- export + package config ------------------------------------------------
install(EXPORT xmedia-kitTargets
    FILE xmedia-kitTargets.cmake
    NAMESPACE xmedia::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/xmedia-kit")

# Written at configure time so a consumer's find_package(xmedia-kit) sees the
# same FFmpeg requirement this build had -- not a guess.
set(XMEDIA_WITH_FFMPEG "${XMEDIA_ENABLE_FFMPEG}")
configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/XmediaConfig.cmake.in"
    "${CMAKE_BINARY_DIR}/generated/xmedia-kitConfig.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/xmedia-kit")
write_basic_package_version_file(
    "${CMAKE_BINARY_DIR}/generated/xmedia-kitConfigVersion.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion)
install(FILES
    "${CMAKE_BINARY_DIR}/generated/xmedia-kitConfig.cmake"
    "${CMAKE_BINARY_DIR}/generated/xmedia-kitConfigVersion.cmake"
    # The exported Targets.cmake carries find_dependency(FFmpeg) calls (the
    # xmedia_ffmpeg target links FFmpeg::* IMPORTED targets PUBLIC), and CMake
    # ships no FindFFmpeg module of its own -- so the module travels with the
    # package, next to the config that needs it. FFmpegVersionProbe.c is the
    # C probe FindFFmpeg.cmake try_compiles to tell FFmpeg majors apart; it
    # resolves relative to the module, so it ships in the same directory.
    # FindLibyuv.cmake rides along for the same reason one level down:
    # xmedia_ffmpeg links libyuv::libyuv (YUV conversion), and without the
    # module the exported link interface names a target no consumer can find.
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindFFmpeg.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FFmpegVersionProbe.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindLibyuv.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/xmedia-kit")

# ---- CPack ------------------------------------------------------------------
# TGZ on every host (the portable artifact), DEB where dpkg exists. Package
# metadata comes from VERSION.txt / project(), so a release tag is the only
# input needed to cut a new one.
set(CPACK_PACKAGE_NAME "xmedia-kit")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_VENDOR "xmedia-kit authors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Chromium-style layered media player core")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_GENERATOR "TGZ")
if(UNIX AND NOT APPLE)
  list(APPEND CPACK_GENERATOR "DEB")
  set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
endif()
include(CPack)

# render/ — the graphics-API layer, published as the standalone module
# xmedia_gl. Two axes meet in the tree and they are NOT the same axis:
#
#   graphics API   render/gl/  (later render/metal/, render/d3d11/ ...)
#   window system  platform/sdl2, platform/glfw, platform/egl
#
# This used to be platform/gl/, a sibling of the window-system backends. That
# flattens both axes into one directory level, and a flat layout does not
# compose: a second graphics API would demand platform/{gl,metal}x{sdl2,glfw,egl}
# = six directories, including combinations that cannot exist. See docs/02 §6.
#
# It also failed that section's own test for belonging in platform/: platform/
# holds code the HOST SELECTS, and nothing selects this layer. Every backend
# links it unconditionally, it carries no window-system SDK at all (the window
# system arrives only as NativeGLContext::GetProcAddress), and it behaves
# identically on every host. Those are the same three reasons FFmpeg left
# platform/ for media/ffmpeg/ (decision D11).
#
# It cannot move into media/ either: invariant C5 forbids third-party platform
# headers under media/. So it is a layer of its own.
#
# ZERO external dependencies, by design. Not on media, not on base, not on the
# platform SDK, not on anything a consumer has to go and fetch. That is what
# makes it usable as a library in its own right: a project can take xmedia_gl --
# context abstraction, entry-point loader, programs, textures, the unit quad,
# shader presets -- without taking a media framework, a window system or a
# logging library along with it. Anything shaped by VideoFrame lives in
# media/gl/ instead.
#
# The GL enumerants and entry points are glad's output, checked in under
# gl/third_party/glad/ -- generated, not fetched. "Vendored" and "a dependency"
# are different things: the build still needs no network, no Python and no glad
# installation, which is the property this module is sold on.
#
# Direction: platform -> render/gl, and media/gl -> render/gl. Nothing in
# render/ may include platform/ or media/. The window system reaches this layer
# only through the NativeGLContext interface each backend implements
# (render/gl/native_gl_context.h).
file(GLOB XMEDIA_GL_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/gl/*.cc")

# Vendored glad -- the generated loader the module's entry-point table and
# enumerants now come from (see gl/third_party/glad/README.md for the exact
# regeneration command). It is built as its own object library rather than
# dropped into the source list below because the project's warning policy
# (-Werror, plus a pile of -fno-* options the C compiler does not accept) has no
# claim on code nobody here wrote: a warning in generated output is a reason to
# regenerate, not to patch it.
set(XMEDIA_GLAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gl/third_party/glad")

add_library(xmedia_gl_vendor OBJECT "${XMEDIA_GLAD_DIR}/src/gl.c")
target_include_directories(xmedia_gl_vendor PRIVATE "${XMEDIA_GLAD_DIR}/include")

# The objects come in through the generator expression rather than through
# target_link_libraries: a link dependency would drag xmedia_gl_vendor into
# install(EXPORT) as a required companion target, and a one-file implementation
# detail has no business in an installed package's interface.
add_library(xmedia_gl ${XMEDIA_GL_SOURCES} $<TARGET_OBJECTS:xmedia_gl_vendor>)
add_library(xmedia::gl ALIAS xmedia_gl)

# The alias above only exists inside this build. Exporting without this line
# would produce xmedia::xmedia_gl (namespace + target name), so an external
# consumer's target_link_libraries() would have to spell the module twice.
# EXPORT_NAME decides what the installed target is called; the in-tree name
# stays xmedia_gl because the directory is render/gl and the module is a
# graphics-API layer, not a second media library.
set_target_properties(xmedia_gl PROPERTIES EXPORT_NAME gl)

# The build interface points at the source root so in-tree users keep writing
# "render/gl/gl_api.h"; the install interface strips that prefix down to the
# module's own include root, so an external consumer writes the same path after
# find_package(xmedia_gl).
#
# The second pair is glad's own include root, and it is PUBLIC because our
# headers include <glad/gl.h> -- gl_api.h hands callers a GladGLContext, so the
# type has to travel with the module. That root also has to be the one that
# carries KHR/khrplatform.h, which glad's header includes as <KHR/...>.
target_include_directories(xmedia_gl PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
    $<BUILD_INTERFACE:${XMEDIA_GLAD_DIR}/include>
    # INSTALL_INTERFACE points at the package's include root (no "xmedia-kit"
    # prefix): the sources write "render/gl/gl_api.h" and the installed tree
    # keeps that exact layout (see XmediaInstall.cmake, which installs the
    # whole render/ tree to include/render). A former "include/xmedia-kit" prefix
    # here duplicated the render/gl headers under include/xmedia-kit/render/gl and
    # diverged from the rest of the package.
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/render/gl/third_party/glad/include>)

# Only this module's own sources define it, so XMEDIA_GL_EXPORT is a no-op for
# everyone else (see render/gl/gl_export.h).
target_compile_definitions(xmedia_gl PRIVATE XMEDIA_GL_IMPLEMENTATION=1)

xmedia_configure_target(xmedia_gl)

# Packaging. Guarded on the top-level build: when xmedia-kit is pulled in with
# add_subdirectory() the parent owns the install rules, and duplicating them
# here would install into the parent's prefix.
if(XMEDIA_IS_TOP_LEVEL)
  include(CMakePackageConfigHelpers)

  install(TARGETS xmedia_gl
      EXPORT xmedia-kitGlTargets
      ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
      LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
      RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
  # Headers land under include/render/gl -- matching the layout XmediaInstall
  # produces for the whole render/ tree, so the same "render/gl/gl_api.h"
  # include works whether a consumer takes the full xmedia-kit package or just
  # find_package(xmedia_gl). No "xmedia-kit" prefix: that would duplicate the
  # headers and break the "installed tree keeps the source layout" guarantee.
  install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/gl/"
      DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/render/gl"
      FILES_MATCHING PATTERN "*.h")
  install(EXPORT xmedia-kitGlTargets
      FILE xmedia-kitGlTargets.cmake
      NAMESPACE xmedia::
      DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/xmedia_gl")

  configure_package_config_file(
      "${CMAKE_SOURCE_DIR}/cmake/xmedia_glConfig.cmake.in"
      "${CMAKE_CURRENT_BINARY_DIR}/xmedia_glConfig.cmake"
      INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/xmedia_gl")
  install(FILES "${CMAKE_CURRENT_BINARY_DIR}/xmedia_glConfig.cmake"
      DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/xmedia_gl")
endif()

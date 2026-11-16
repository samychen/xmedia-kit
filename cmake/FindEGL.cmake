# FindEGL — xmedia-kit's own module; CMake ships none (3.22 has no FindEGL,
# and FindOpenGL does not cover EGL), which is why the EGL backend silently
# skipped on every host until this file existed.
#
# Produces:
#   EGL_FOUND / <EGL_FOUND>   — both spellings, per find_package convention
#   EGL::EGL                  — imported target (location + include dir)
#   EGL_INCLUDE_DIRS, EGL_LIBRARIES — the plain variables, for callers that
#                               do not use imported targets.
#
# The header set is EGL/egl.h plus KHR/khrplatform.h; eglext.h is not required
# to exist for the find to succeed but is present on every real EGL SDK, and
# code that needs it (the dmabuf importer) fails at compile time rather than
# configure time on a broken install.

find_path(EGL_INCLUDE_DIR NAMES EGL/egl.h
    HINTS ${EGL_ROOT} $ENV{EGL_ROOT})
find_library(EGL_LIBRARY NAMES EGL
    HINTS ${EGL_ROOT} $ENV{EGL_ROOT})

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(EGL DEFAULT_MSG
    EGL_LIBRARY EGL_INCLUDE_DIR)

mark_as_advanced(EGL_INCLUDE_DIR EGL_LIBRARY)

if(EGL_FOUND AND NOT TARGET EGL::EGL)
  add_library(EGL::EGL UNKNOWN IMPORTED)
  set_target_properties(EGL::EGL PROPERTIES
      IMPORTED_LOCATION "${EGL_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${EGL_INCLUDE_DIR}")
  set(EGL_INCLUDE_DIRS "${EGL_INCLUDE_DIR}")
  set(EGL_LIBRARIES "${EGL_LIBRARY}")
endif()

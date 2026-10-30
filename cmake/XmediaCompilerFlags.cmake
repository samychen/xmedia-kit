# Copyright 2026 The xmedia-kit Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

include(CheckCXXCompilerFlag)
include(CheckCXXSourceCompiles)

if(XMEDIA_BUILD_SHARED)
  if(WIN32)
    set(XMEDIA_DLLEXPORT "__declspec(dllexport)")
    set(XMEDIA_DLLIMPORT "__declspec(dllimport)")
  else()
    set(XMEDIA_DLLEXPORT "__attribute__((visibility(\"default\")))")
    set(XMEDIA_DLLIMPORT "__attribute__((visibility(\"default\")))")
  endif()
else()
  set(XMEDIA_DLLEXPORT "")
  set(XMEDIA_DLLIMPORT "")
endif()

set(XMEDIA_WARNINGS_COMMON
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual -Wunused
    -Wcast-align -Wnull-dereference -Wdouble-promotion
    -Wimplicit-fallthrough -Wformat=2
    -Werror=return-type -Werror=uninitialized -Werror=parentheses
    -Werror=narrowing -Werror=delete-non-virtual-dtor -Werror=reorder
    -Wno-unknown-pragmas)
set(XMEDIA_WARNINGS_STRICT
    -Wconversion -Wsign-conversion -Wold-style-cast
    -Wsuggest-override -Wzero-as-null-pointer-constant -Wextra-semi
    -Wpessimizing-move)
# GCC-only. clang rejects -Wuseless-cast as an unknown option, which together
# with XMEDIA_WERROR makes the strict/debug preset -- the configuration
# docs/BUILDING.md points reviewers at -- fail on the first translation unit
# instead of compiling the tree. There is no clang equivalent to fall back to.
set(XMEDIA_WARNINGS_STRICT_GCC_ONLY
    -Wuseless-cast)

# The ABI half of xmedia_configure_target(): every macro that changes how a
# type in a PUBLIC HEADER is laid out.
#
# This is separate from the function below because a host application cannot
# always take the library's -fno-exceptions / -fno-rtti (Qt's meta-object
# system needs RTTI), but it MUST still see identical layouts. Getting this
# wrong is silent and expensive: SEQUENCE_CHECKER expands to a member when
# XMEDIA_ENABLE_DCHECK is defined and to nothing when it is not, so a
# VideoFrame built by the library (one layout) and released through an inline
# scoped_refptr in the app (the other layout) AddRef()s the wrong field -- the
# refcount never grows, the frame is freed while the compositor still holds it,
# and the crash lands in heap-allocator internals somewhere unrelated.
#
# Measured on apps/player_qt: AddressSanitizer reported heap-use-after-free in
# VideoFrameCompositor::Render, 40 bytes into a 232-byte VideoFrame, freed from
# the app's paint path. Adding these definitions fixed it.
function(xmedia_configure_abi_definitions target)
  target_compile_definitions(${target} PRIVATE
      $<$<BOOL:${XMEDIA_ENABLE_DCHECK}>:XMEDIA_ENABLE_DCHECK=1>
      $<$<CONFIG:Debug>:XMEDIA_DEBUG=1>)
  check_cxx_compiler_flag(-Wthread-safety _xmedia_has_thread_safety)
  if(_xmedia_has_thread_safety)
    target_compile_definitions(${target} PRIVATE
        XMEDIA_THREAD_SAFETY_ANALYSIS=1)
  endif()
endfunction()

function(xmedia_configure_target target)
  target_compile_features(${target} PUBLIC cxx_std_20)

  # Google / Chromium: exceptions and RTTI are disabled project-wide.
  if(NOT MSVC)
    target_compile_options(${target} PRIVATE -fno-exceptions -fno-rtti)
    target_compile_definitions(${target} PRIVATE XMEDIA_NO_EXCEPTIONS=1)
  else()
    target_compile_options(${target} PRIVATE /GR- /EHs-c-)
    target_compile_definitions(${target} PRIVATE _HAS_EXCEPTIONS=0)
  endif()

  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive- /Zc:__cplusplus
        /wd4251 $<$<BOOL:${XMEDIA_WERROR}>:/WX>)
  else()
    target_compile_options(${target} PRIVATE ${XMEDIA_WARNINGS_COMMON}
        $<$<BOOL:${XMEDIA_STRICT_WARNINGS}>:${XMEDIA_WARNINGS_STRICT}>
        $<$<AND:$<BOOL:${XMEDIA_STRICT_WARNINGS}>,$<CXX_COMPILER_ID:GNU>>:${XMEDIA_WARNINGS_STRICT_GCC_ONLY}>
        $<$<BOOL:${XMEDIA_WERROR}>:-Werror>)
    check_cxx_compiler_flag(-Wthread-safety XMEDIA_HAS_THREAD_SAFETY)
    if(XMEDIA_HAS_THREAD_SAFETY)
      target_compile_options(${target} PRIVATE -Wthread-safety)
    endif()
  endif()

  xmedia_configure_abi_definitions(${target})

  if(XMEDIA_SANITIZERS)
    set(_sf "")
    foreach(s IN LISTS XMEDIA_SANITIZERS)
      list(APPEND _sf "-fsanitize=${s}")
    endforeach()
    target_compile_options(${target} PRIVATE ${_sf} -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE ${_sf})
  endif()

  if(XMEDIA_COVERAGE)
    target_compile_options(${target} PRIVATE --coverage -O0 -g)
    target_link_options(${target} PRIVATE --coverage)
  endif()

  target_include_directories(${target} SYSTEM PRIVATE
      "${CMAKE_BINARY_DIR}/generated")
endfunction()

check_cxx_source_compiles("
  #include <expected>
  int main() { std::expected<int, int> e{1}; return *e - 1; }"
  XMEDIA_HAVE_STD_EXPECTED)

if(XMEDIA_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT XMEDIA_IPO_OK OUTPUT XMEDIA_IPO_MSG)
  if(XMEDIA_IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO TRUE)
  else()
    message(WARNING "LTO requested but unsupported: ${XMEDIA_IPO_MSG}")
  endif()
endif()

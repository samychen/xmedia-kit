# base/ — the Chromium-shaped foundation layer. Zero external dependencies:
# this target must build and its tests must pass on a machine with nothing but
# a C++20 compiler (design goal G2).
#
# Source listing stays explicit (docs/06 §7.6): this is the layer whose
# abstraction is one file per platform -- threading/platform_thread_posix.cc
# today, the posix/win pairs docs/06 §7.1 plans for synchronization/ later -- so
# directory membership is not target membership, the same reason media/filters/
# is hand-written. A glob here would compile the wrong platform's file.
add_library(xmedia_base
    logging.cc
    check.cc
    synchronization/lock.cc
    task/task_runner.cc
    task/sequenced_task_runner.cc
    task/task_queue.cc
    threading/platform_thread_posix.cc
    threading/thread.cc
    time/time.cc
    time/default_tick_clock.cc)
add_library(xmedia::base ALIAS xmedia_base)

target_include_directories(xmedia_base PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
    $<BUILD_INTERFACE:${CMAKE_BINARY_DIR}/generated>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/xmedia-kit>)
target_link_libraries(xmedia_base PUBLIC Threads::Threads)
target_compile_definitions(xmedia_base PRIVATE XMEDIA_BASE_IMPLEMENTATION=1)
xmedia_configure_target(xmedia_base)

# Test-only support library, kept separate from xmedia_base so it can never
# reach a shipped binary.
if(XMEDIA_BUILD_TESTS)
  add_library(xmedia_base_test_support test/task_environment.cc)
  add_library(xmedia::base_test_support ALIAS xmedia_base_test_support)
  target_include_directories(xmedia_base_test_support PUBLIC
      $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>)
  target_link_libraries(xmedia_base_test_support PUBLIC xmedia::base)
  target_compile_definitions(xmedia_base_test_support PRIVATE
      XMEDIA_BASE_IMPLEMENTATION=1)
  xmedia_configure_target(xmedia_base_test_support)
endif()

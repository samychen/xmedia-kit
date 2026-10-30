# xmedia-inspect: diagnostic CLI. Requires FFmpeg because it drives the real
# demuxer and decoders -- a version built on fakes would prove nothing.
#
# Split per subcommand so no file exceeds the 500-line limit (invariant C1) and
# no function exceeds 80 lines (C2). inspect_common holds the plumbing all three
# share: argument parsing, the demuxer Host, and the task-queue Pump.
#
# Source listing (docs/06 §7.6): one directory, one target, no per-file option
# or platform gate -- the whole directory is gated where it is added in the
# top-level CMakeLists, not file by file. So the list is a glob and a new
# subcommand needs no edit here.
file(GLOB XMEDIA_INSPECT_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")
add_executable(xmedia-inspect ${XMEDIA_INSPECT_SOURCES})
# xmedia::ffmpeg is NOT listed here: xmedia::transcode links it PUBLIC, so it
# arrives through that. Listing both made the linker complain about a
# duplicate library on every link.
target_link_libraries(xmedia-inspect PRIVATE
    xmedia::transcode
    xmedia::base_test_support)
# The CLI includes platform/ffmpeg headers (av_includes.h), and
# xmedia_ffmpeg links FFmpeg PRIVATE on purpose (it is the only
# FFmpeg-linked target), so the includes must be requested here as well.
find_package(FFmpeg REQUIRED COMPONENTS avformat avcodec avutil)
target_link_libraries(xmedia-inspect PRIVATE
    FFmpeg::avformat FFmpeg::avcodec FFmpeg::avutil)
target_include_directories(xmedia-inspect PRIVATE ${CMAKE_SOURCE_DIR})
xmedia_configure_target(xmedia-inspect)

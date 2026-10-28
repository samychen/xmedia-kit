// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// `avbase-inspect preview`: the seek-preview batch grab, written to disk.
//
// Why a CLI and not just the library call: PreviewFrames() returned frames
// to nobody before this existed -- its only caller was its own unit test, and
// a converter nobody runs is exactly where the 10-bit/NV12 pixel bug lived
// undetected. Writing the frames out as JPEGs makes the OUTPUT inspectable:
// you can look at the picture, and `--expect-ssim` turns "looks right" into a
// number a gate can assert.
//
// It reuses WriteJpegSnapshot (the TakeSnapshot backend), so the bytes that
// land on disk come from the same encoder the SDK uses for a single snapshot.

#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "media/base/video_frame.h"
#include "media/ffmpeg/ffmpeg_image_snapshot.h"
#include "media/transcode/ffmpeg_preview_job.h"
#include "tools/inspect/inspect_common.h"

namespace avbase {
namespace {

// Prints the frames' geometry, timestamps and the gaps between them. The
// gaps are the part worth reading: a preview strip whose frames are 0.1 s
// apart when they were asked to be 1 s apart is a seek bug, and it is
// invisible in the frame count.
void ReportFrames(
    const std::vector<base::scoped_refptr<media::VideoFrame>>& frames) {
  printf("frames: %zu\n", frames.size());
  if (frames.empty()) {
    return;
  }
  const media::VideoFrame& first = *frames.front();
  printf("geometry: %dx%d\n", first.coded_size().width,
         first.coded_size().height);
  printf("format: I420 (8-bit; sources in other formats are converted)\n");
  int64_t previous_ms = -1;
  for (size_t i = 0; i < frames.size(); ++i) {
    const int64_t ms = frames[i]->timestamp().InMilliseconds();
    printf("  [%zu] t=%" PRId64 " ms", i, ms);
    if (previous_ms >= 0) {
      printf("  (+%" PRId64 " ms)", ms - previous_ms);
    }
    printf("\n");
    previous_ms = ms;
  }
}

}  // namespace

int RunPreview(int argc, char** argv) {
  std::string input;
  std::string out_dir;
  std::string prefix = "preview";
  double start_seconds = 0.0;
  int count = 1;
  // 0 = grab the next |count| frames as they come (the natural strip for a
  // scrubber). A positive interval spaces the grab points out instead, which
  // is what a hover-preview wants: seeking per frame is the honest way to
  // simulate that, since PreviewFrames is a keyframe seek each time.
  double interval_seconds = 0.0;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      Usage();
      return 0;
    }
    if (arg == "--verbose") {
      base::logging::SetMinLogLevel(base::logging::LOG_INFO);
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      if (arg != "-o" && arg != "--output" && arg != "--prefix" &&
          arg != "--start" && arg != "--count" && arg != "--interval") {
        fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
        return 1;
      }
      if (i + 1 >= argc) {
        fprintf(stderr, "error: %s needs a value\n", arg.c_str());
        return 1;
      }
      const char* value = argv[++i];
      if (arg == "-o" || arg == "--output") {
        out_dir = value;
      } else if (arg == "--prefix") {
        prefix = value;
      } else if (arg == "--start") {
        if (!ParseSecondsArg(value, /*allow_zero=*/true, &start_seconds)) {
          fprintf(stderr,
                  "error: --start '%s' is not a non-negative number of "
                  "seconds\n",
                  value);
          return 1;
        }
      } else if (arg == "--count") {
        if (!ParseCountArg(value, 100000, &count)) {
          fprintf(stderr, "error: --count '%s' is not a positive frame count\n",
                  value);
          return 1;
        }
      } else {
        if (!ParseSecondsArg(value, /*allow_zero=*/false, &interval_seconds) ||
            interval_seconds <= 0.0) {
          fprintf(stderr,
                  "error: --interval '%s' is not a positive number of "
                  "seconds\n",
                  value);
          return 1;
        }
      }
      continue;
    }
    if (input.empty()) {
      input = arg;
    } else {
      fprintf(stderr, "error: unexpected argument '%s'\n", arg.c_str());
      return 1;
    }
  }
  if (input.empty()) {
    fprintf(stderr, "error: preview needs an input file\n");
    return 1;
  }
  if (out_dir.empty()) {
    fprintf(stderr,
            "error: preview needs -o <directory> (the frames are written as "
            "JPEGs)\n");
    return 1;
  }
  if (count <= 0) {
    fprintf(stderr, "error: --count must be positive\n");
    return 1;
  }

  printf("preview: %s -> %s/%s_*.jpg\n", input.c_str(), out_dir.c_str(),
         prefix.c_str());
  printf("  start: %.3f s   count: %d", start_seconds, count);
  if (interval_seconds > 0.0) {
    printf("   interval: %.3f s", interval_seconds);
  } else {
    printf("   interval: consecutive frames");
  }
  printf("\n");

  // The output directory is created rather than required to pre-exist: a
  // preview strip is a scratch product, and "make the directory for me" is
  // the behaviour every other CLI with this flag has. Failing here would turn
  // a missing mkdir into a preview that "did not work".
  std::error_code mkdir_error;
  std::filesystem::create_directories(out_dir, mkdir_error);
  if (mkdir_error && !std::filesystem::is_directory(out_dir)) {
    fprintf(stderr, "error: cannot create output directory '%s': %s\n",
            out_dir.c_str(), mkdir_error.message().c_str());
    return 1;
  }

  std::vector<base::scoped_refptr<media::VideoFrame>> frames;
  if (interval_seconds > 0.0) {
    // Spaced grabs: one PreviewFrames call per point. Deliberately not a
    // single call with a stride -- the product feature is "hover here, show
    // this", which is one seek per point, and a batch API that only ever
    // ran in consecutive mode would never exercise the seek path.
    for (int i = 0; i < count; ++i) {
      std::vector<base::scoped_refptr<media::VideoFrame>> one;
      const double at = start_seconds + interval_seconds * i;
      const media::Status status = media::PreviewFrames(input, at, 1, &one);
      if (!status) {
        fprintf(stderr, "error: frame %d at %.3fs: %s\n", i, at,
                status.error().ToString().c_str());
        return 1;
      }
      frames.push_back(one.front());
    }
  } else {
    const media::Status status =
        media::PreviewFrames(input, start_seconds, count, &frames);
    if (!status) {
      fprintf(stderr, "error: %s\n", status.error().ToString().c_str());
      return 1;
    }
  }

  ReportFrames(frames);

  for (size_t i = 0; i < frames.size(); ++i) {
    char name[64];
    snprintf(name, sizeof(name), "/%s_%03zu.jpg", prefix.c_str(), i);
    const std::string path = out_dir + name;
    if (const media::Status write = media::WriteJpegSnapshot(*frames[i], path);
        !write) {
      fprintf(stderr, "error: cannot write '%s': %s\n", path.c_str(),
              write.error().ToString().c_str());
      return 1;
    }
    printf("wrote %s\n", path.c_str());
  }
  return 0;
}

}  // namespace avbase

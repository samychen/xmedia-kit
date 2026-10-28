// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// `avbase-inspect record`: capture a source to a file for a bounded window.
//
// WHAT THIS IS, AND WHAT IT IS NOT. This records by READING THE SOURCE, not by
// tapping a playing pipeline. The difference matters and is why the summary
// says so out loud: for a file or a live URL the bytes are identical, but a
// host that wants "record exactly what the viewer saw" (including a seek the
// viewer did, or a track they switched away from) needs a tap on the live
// pipeline, which does not exist yet. Recording by re-reading is the honest
// first step: it is the whole streaming/recording stack (network open, live
// demux, pacing, mux) with no new plumbing in the playback path, so the
// format/container/pacing questions get answered before the pipeline grows an
// extension point for them.
//
// Copy by default. Recording is usually "keep the bits that arrived", and
// re-encoding a live stream on a machine that is also trying to keep up is how
// a recorder drops frames. --vcodec/--acodec exist for the cases that need
// them (a container that cannot hold the source codec, or a size budget).
//
// Stopping: --duration is a wall-clock budget for a live source and a media
// budget for a file. For a live stream the two are the same thing while
// playback keeps up; when it does not, the wall clock is what the user asked
// for ("record 30 seconds"), so it wins.

#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "base/logging.h"
#include "media/transcode/ffmpeg_transcode_job.h"
#include "tools/inspect/inspect_common.h"

namespace avbase {
namespace {

bool ParseSeconds(const char* text, double* out) {
  if (text == nullptr || text[0] == '\0') {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const double parsed = strtod(text, &end);
  if (end == text || *end != '\0' || errno == ERANGE ||
      !std::isfinite(parsed) || parsed <= 0.0) {
    return false;
  }
  *out = parsed;
  return true;
}

}  // namespace

int RunRecord(int argc, char** argv) {
  std::string input;
  std::string output;
  std::string format;
  std::string vcodec = "copy";
  std::string acodec = "copy";
  double duration_seconds = 0.0;
  bool no_realtime = false;

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
    if (arg == "--no-realtime") {
      no_realtime = true;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: %s needs a value\n", arg.c_str());
        return 1;
      }
      const char* value = argv[++i];
      if (arg == "-o" || arg == "--output") {
        output = value;
      } else if (arg == "--format") {
        format = value;
      } else if (arg == "--vcodec") {
        vcodec = value;
      } else if (arg == "--acodec") {
        acodec = value;
      } else if (arg == "--duration") {
        if (!ParseSeconds(value, &duration_seconds)) {
          fprintf(stderr,
                  "error: --duration '%s' is not a positive number of "
                  "seconds\n",
                  value);
          return 1;
        }
      } else {
        fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
        return 1;
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
    fprintf(stderr, "error: record needs an input (a file or a live URL)\n");
    return 1;
  }
  if (output.empty()) {
    fprintf(stderr, "error: record needs -o <output>\n");
    return 1;
  }
  // An unbounded recording of a live source runs until killed. Allowed, but
  // said out loud: the alternatives are "refuse" (a legitimate use) and
  // "silently record forever" (a surprise).
  if (duration_seconds <= 0.0) {
    printf(
        "note: no --duration given; recording runs until interrupted or "
        "the source ends\n");
  }

  media::TranscodeParams params;
  params.output_path = output;
  params.output_format = format;
  params.duration_seconds = duration_seconds;
  params.video_codec.codec = vcodec == "none" ? std::string() : vcodec;
  params.audio_codec.codec = acodec == "none" ? std::string() : acodec;
  // Realtime on by default: a live source delivers at its own rate anyway, and
  // silencing the pacer is the caller's call, not a surprise.
  params.realtime = !no_realtime;

  printf("record: %s -> %s\n", input.c_str(), output.c_str());
  printf("  container: %s   video: %s   audio: %s\n",
         format.empty() ? "(from extension)" : format.c_str(), vcodec.c_str(),
         acodec.c_str());
  printf("  limit: %s\n",
         duration_seconds > 0.0 ? "--duration applies" : "unbounded");

  const auto started = std::chrono::steady_clock::now();
  const media::Status status = media::Transcode(input, params, nullptr);
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  if (!status) {
    fprintf(stderr, "error: record failed: %s\n",
            status.error().ToString().c_str());
    return 1;
  }
  printf("recorded %.2fs wall\n", elapsed);
  printf(
      "note: this read the source; it did not tap a playing session (see "
      "docs/TODO §1)\n");
  return 0;
}

}  // namespace avbase

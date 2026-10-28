// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "tools/inspect/inspect_common.h"

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "media/base/data_source_descriptor.h"

namespace avbase {

// The single copy of the numeric parsers the subcommands used to redefine.
// Both are strict on purpose: atof/atoi turn garbage into 0, which reads as a
// successful run with a nonsense parameter (the L9 lesson, still true).
media::TranscodeParams BuildJobParams(const std::string& output,
                                      const std::string& format,
                                      const std::string& vcodec,
                                      const std::string& acodec,
                                      double duration, bool realtime) {
  media::TranscodeParams params;
  params.output_path = output;
  params.output_format = format;
  params.duration_seconds = duration;
  // "none" and empty both mean "drop the stream" (ffmpeg's -vn/-an); anything
  // else, including "copy", is passed through untouched.
  params.video_codec.codec =
      (vcodec == "none" || vcodec.empty()) ? std::string() : vcodec;
  params.audio_codec.codec =
      (acodec == "none" || acodec.empty()) ? std::string() : acodec;
  params.realtime = realtime;
  return params;
}

bool ParseSecondsArg(const char* text, bool allow_zero, double* out) {
  if (text == nullptr || text[0] == '\0' || out == nullptr) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const double parsed = strtod(text, &end);
  // strtod also accepts "nan"/"inf", neither of which is a time point.
  if (end == text || *end != '\0' || errno == ERANGE ||
      !std::isfinite(parsed)) {
    return false;
  }
  if (parsed < 0.0 || (!allow_zero && parsed == 0.0)) {
    return false;
  }
  *out = parsed;
  return true;
}

bool ParseCountArg(const char* text, int max_value, int* out) {
  if (text == nullptr || text[0] == '\0' || out == nullptr) {
    return false;
  }
  errno = 0;
  char* end = nullptr;
  const int64_t parsed = strtoll(text, &end, 10);
  // strtoll accepts leading whitespace and a sign; neither is a count.
  if (end == text || *end != '\0' || errno == ERANGE || parsed <= 0) {
    return false;
  }
  if (max_value > 0 && parsed > max_value) {
    return false;
  }
  *out = static_cast<int>(parsed);
  return true;
}

const char* StreamKindName(media::StreamKind kind) {
  switch (kind) {
  case media::StreamKind::kVideo:
    return "video";
  case media::StreamKind::kAudio:
    return "audio";
  case media::StreamKind::kText:
    return "text";
  case media::StreamKind::kUnknown:
    return "unknown";
  }
  return "unknown";
}

void Usage() {
  printf(
      "avbase-inspect -- diagnostics over the real avbase media stack\n"
      "\n"
      "usage:\n"
      "  avbase-inspect probe  <file>\n"
      "      Print container and per-stream information.\n"
      "\n"
      "  avbase-inspect decode <file> [--video|--audio] [--limit N]\n"
      "      Demux and decode, then report per-stream frame counts,\n"
      "      timestamp range and monotonicity.\n"
      "\n"
      "  avbase-inspect sync   <file> [--limit N]\n"
      "      Decode both streams and drive AvSyncController with the\n"
      "      real timestamps, printing the resolved master clock and\n"
      "      the audio sample correction at each step.\n"
      "\n"
      "  avbase-inspect transcode <input> -o <output> [options]\n"
      "      Run the offline transcode line (media/transcode) end to end,\n"
      "      then re-open the output to prove it is readable media.\n"
      "\n"
      "    -o, --output PATH   output file; the container follows the\n"
      "                       extension (required)\n"
      "    --vcodec NAME      video encoder family (default mjpeg; \"copy\"\n"
      "                       for stream copy; libx264 where built)\n"
      "    --acodec NAME      audio encoder (default aac; \"copy\")\n"
      "    --no-video         drop the video stream (-vn)\n"
      "    --no-audio         drop the audio stream (-an)\n"
      "    --start SECONDS    trim: seek here before encoding\n"
      "    --duration SECONDS trim: stop after this much output\n"
      "    --crf N            quality-driven bitrate (software encoders)\n"
      "    --preset NAME      encoder preset (e.g. veryfast)\n"
      "    --vbitrate N       video bitrate in bits/s\n"
      "    --abitrate N       audio bitrate in bits/s\n"
      "    --width N          scale output (with --height)\n"
      "    --height N\n"
      "    --fps N            output frame rate\n"
      "    --hw               prefer a hardware encoder when one exists\n"
      "    --encopt KEY=VALUE private encoder option, repeatable\n"
      "                       (e.g. allow_sw=1 for h264_videotoolbox on a\n"
      "                       machine with no VT session)\n"
      "    --faststart        movflags=+faststart (mp4)\n"
      "\n"
      "  avbase-inspect preview <input> -o <dir> [options]\n"
      "      Grab a batch of frames around a seek point and write them as\n"
      "      JPEGs (seek preview / filmstrip). Reuses TakeSnapshot's encoder.\n"
      "\n"
      "    -o, --output DIR   directory to write into (required)\n"
      "    --start SECONDS    seek here first (keyframe seek)\n"
      "    --count N          how many frames (default 1)\n"
      "    --interval SECONDS grab one frame every SECONDS instead of\n"
      "                       consecutive frames (one seek per point)\n"
      "    --prefix NAME      file name prefix (default \"preview\")\n"
      "\n"
      "  avbase-inspect publish <input> -o <url> [options]\n"
      "      Push a file to an RTMP endpoint (or write any container).\n"
      "      A URL has no extension, so --format is required for rtmp://.\n"
      "\n"
      "    -o, --output URL   destination (rtmp://host/app/key)\n"
      "    --format NAME      container; \"flv\" for RTMP (required for URLs)\n"
      "    --vcodec NAME      video encoder (default copy = no re-encode)\n"
      "    --acodec NAME      audio encoder (default copy)\n"
      "    --no-video         drop the video stream\n"
      "    --no-audio         drop the audio stream\n"
      "    --no-realtime      do not pace against the wall clock (offline\n"
      "                       targets want this; live peers do not)\n"
      "    --encopt KEY=VALUE private encoder option, repeatable\n"
      "\n"
      "  avbase-inspect record <input> -o <output> [options]\n"
      "      Capture a file or a live URL to a file for a bounded window.\n"
      "      Reads the SOURCE; it does not tap a playing session.\n"
      "\n"
      "    -o, --output PATH  destination file\n"
      "    --duration SECONDS stop after this much (wall clock for a\n"
      "                       live source); unbounded when omitted\n"
      "    --format NAME      container when the output has no extension\n"
      "    --vcodec NAME      video encoder (default copy); \"none\" drops\n"
      "    --acodec NAME      audio encoder (default copy); \"none\" drops\n"
      "    --no-realtime      ignore the source clock (offline targets)\n"
      "\n"
      "options:\n"
      "  --limit N   Stop after N decoded outputs per stream (default "
      "100000).\n"
      "  --verbose   Enable avbase INFO logging.\n"
      "  -h, --help  This text.\n");
}

bool ParseArgs(int argc, char** argv, Options* out) {
  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      Usage();
      return false;
    } else if (arg == "--video") {
      out->video_only = true;
    } else if (arg == "--audio") {
      out->audio_only = true;
    } else if (arg == "--verbose") {
      base::logging::SetMinLogLevel(base::logging::LOG_INFO);
    } else if (arg == "--limit") {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: --limit needs a value\n");
        return false;
      }
      // L9: atol() silently parsed garbage as 0 and wrapped negatives into
      // huge size_t values; validate instead.
      const char* value = argv[++i];
      char* end = nullptr;
      errno = 0;
      // strtoull silently accepts a leading '-' and wraps; reject it first.
      if (value[0] == '-') {
        fprintf(stderr, "error: --limit '%s' is not a valid count\n", value);
        return false;
      }
      // LP64: uint64_t and size_t are both 64-bit, so ERANGE is the only
      // overflow signal; the cast below cannot truncate.
      const uint64_t parsed = strtoull(value, &end, 10);
      if (end == value || *end != '\0' || errno == ERANGE) {
        fprintf(stderr, "error: --limit '%s' is not a valid count\n", value);
        return false;
      }
      out->limit = static_cast<size_t>(parsed);
    } else if (!arg.empty() && arg[0] == '-') {
      fprintf(stderr, "error: unknown option '%s'\n", arg.c_str());
      return false;
    } else if (out->subcommand.empty()) {
      out->subcommand = arg;
    } else if (out->path.empty()) {
      out->path = arg;
    } else {
      fprintf(stderr, "error: unexpected argument '%s'\n", arg.c_str());
      return false;
    }
  }
  if (out->subcommand.empty()) {
    Usage();
    return false;
  }
  if (out->path.empty()) {
    fprintf(stderr, "error: %s needs a media file\n", out->subcommand.c_str());
    return false;
  }
  return true;
}

// Host that prints demuxer errors instead of swallowing them. A silent Host
bool OpenDemuxer(media::FFmpegDemuxer* demuxer, const std::string& path,
                 InspectHost* host, Pump* pump,
                 base::scoped_refptr<base::SequencedTaskRunner> runner,
                 const media::MediaInfo** info) {
  media::Status result =
      media::Err(media::ErrorCode::kNotImplemented, "not run", {}, {});
  bool done = false;
  demuxer->Initialize(media::DataSourceDescriptor::FromUri(path),
                      media::DemuxerOptions{}, host, runner,
                      base::BindOnce(
                          [](media::Status* out, bool* flag, media::Status s) {
                            *out = std::move(s);
                            *flag = true;
                          },
                          &result, &done));
  if (!pump->Until([&done] { return done; })) {
    fprintf(stderr, "error: opening '%s' timed out\n", path.c_str());
    return false;
  }
  if (!result.has_value()) {
    fprintf(stderr, "error: cannot open '%s'\n%s\n", path.c_str(),
            result.error().ToString().c_str());
    return false;
  }
  *info = &demuxer->media_info();
  return true;
}

}  // namespace avbase

// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// `xmedia-inspect publish`: push a file to an RTMP endpoint.
//
// This is the publishing half of the streaming story (the playing half is the
// rtmp/rtsp/srt input protocols, already accepted). It is deliberately thin:
// Transcode() already does demux -> (encode) -> mux, and a network output is
// just a muxer whose URL has no extension. Three things differ from a file
// output and each has a way to get it wrong:
//
//   * The container cannot be inferred from a URL, so --format is required
//     for anything that is not a local file. Without it the muxer open fails;
//     that failure is propagated, not papered over with a default, because a
//     silent default is how "published" becomes "published the wrong thing".
//   * A publisher must pace itself against the wall clock. The offline job
//     runs as fast as the disk allows; feeding a live endpoint that way makes
//     it drop everything it cannot buffer. --realtime inserts that pacing.
//   * "The muxer returned OK" is not "the peer received it". The caller gets
//     the byte count and, when the container is a file, the re-open check;
//     for a network URL the honest check is the receiving end.
//
// Acceptance for the network path is a loopback run against an FFmpeg
// listener (rtmp supports listen=1), which needs no MediaMTX; see
// tools/check_rtmp_publish.py.

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "base/logging.h"
#include "media/transcode/ffmpeg_transcode_job.h"
#include "tools/inspect/inspect_common.h"

namespace xmedia {
namespace {

bool IsNetworkUrl(const std::string& url) {
  // A scheme before "//" is what makes it a URL; a Windows path like C:\foo
  // has a colon but no "//".
  const size_t scheme = url.find("://");
  return scheme != std::string::npos && scheme > 0;
}

}  // namespace

int RunPublish(int argc, char** argv) {
  std::string input;
  std::string url;
  std::string format;
  std::string vcodec = "copy";
  std::string acodec = "copy";
  bool realtime = true;
  bool drop_video = false;
  bool drop_audio = false;
  std::vector<std::pair<std::string, std::string>> encopts;

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
      realtime = false;
      continue;
    }
    if (arg == "--no-video") {
      drop_video = true;
      continue;
    }
    if (arg == "--no-audio") {
      drop_audio = true;
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      if (i + 1 >= argc) {
        fprintf(stderr, "error: %s needs a value\n", arg.c_str());
        return 1;
      }
      const char* value = argv[++i];
      if (arg == "-o" || arg == "--output" || arg == "--url") {
        url = value;
      } else if (arg == "--format") {
        format = value;
      } else if (arg == "--vcodec") {
        vcodec = value;
      } else if (arg == "--acodec") {
        acodec = value;
      } else if (arg == "--encopt") {
        const std::string pair = value;
        const size_t equals = pair.find('=');
        if (equals == std::string::npos || equals == 0) {
          fprintf(stderr, "error: --encopt '%s' is not KEY=VALUE\n", value);
          return 1;
        }
        encopts.emplace_back(pair.substr(0, equals), pair.substr(equals + 1));
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
    fprintf(stderr, "error: publish needs an input file\n");
    return 1;
  }
  if (url.empty()) {
    fprintf(stderr,
            "error: publish needs -o <url> (e.g. "
            "rtmp://127.0.0.1:1935/live/key)\n");
    return 1;
  }
  // The container is REQUIRED for a network URL: there is no extension to
  // infer from, and FFmpeg's refusal to guess is a feature we forward.
  if (format.empty() && IsNetworkUrl(url)) {
    fprintf(stderr,
            "error: '%s' has no file extension, so the container cannot be "
            "inferred; pass --format (\"flv\" for RTMP)\n",
            url.c_str());
    return 1;
  }

  media::TranscodeParams params = BuildJobParams(
      url, format, drop_video ? std::string() : vcodec,
      drop_audio ? std::string() : acodec, /*duration=*/0.0, realtime);
  params.video_codec.options = encopts;

  printf("publish: %s -> %s\n", input.c_str(), url.c_str());
  printf("  container: %s   video: %s   audio: %s   pacing: %s\n",
         format.empty() ? "(from extension)" : format.c_str(),
         drop_video ? "(dropped)" : vcodec.c_str(),
         drop_audio ? "(dropped)" : acodec.c_str(),
         realtime ? "realtime" : "as fast as possible");

  const auto started = std::chrono::steady_clock::now();
  const media::Status status = media::Transcode(input, params, nullptr);
  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started)
          .count();
  if (!status) {
    fprintf(stderr, "error: publish failed: %s\n",
            status.error().ToString().c_str());
    return 1;
  }
  printf("published in %.2fs\n", elapsed);
  if (!IsNetworkUrl(url)) {
    // A file target can be verified locally; a live peer cannot, and claiming
    // "delivered" from the muxer's return code would be a false success.
    printf(
        "note: '%s' is a local path, not a live endpoint; nothing was "
        "delivered to a peer\n",
        url.c_str());
  }
  return 0;
}

}  // namespace xmedia

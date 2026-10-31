// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// xmedia-inspect: a diagnostic CLI over the real media stack.
//
// Why this exists: legacy's only window into its own behaviour is a wall of
// LOGD lines from ff_ffplay.c, and several classes of bug -- a wrong time base,
// a sign inversion in the sync correction, a decoder that silently produces no
// frames -- are invisible in that output. Each subcommand prints the
// intermediate values a developer would otherwise need a debugger for, using
// exactly the code paths the player uses and no test doubles.
//
// Bug #32 (a master clock offset by negative uptime) was found by `sync` on its
// first run against a real file, after 300+ unit tests had passed.

#include <cstdio>
#include <string>

#include "media/ffmpeg/log_bridge.h"
#include "tools/inspect/inspect_common.h"

int main(int argc, char** argv) {
  // transcode parses its own options: the shared parser rejects unknown flags
  // by design (a silently dropped --limit would make a run look finished when
  // it was not), and transcode's set is a different one (-o, --vcodec, ...).
  // Routing here keeps that refusal intact for the other three subcommands.
  if (argc > 1 && std::string(argv[1]) == "transcode") {
    xmedia::media::ffmpeg::InstallLogBridge();
    return xmedia::RunTranscode(argc - 1, argv + 1);
  }
  if (argc > 1 && std::string(argv[1]) == "preview") {
    xmedia::media::ffmpeg::InstallLogBridge();
    return xmedia::RunPreview(argc - 1, argv + 1);
  }
  if (argc > 1 && std::string(argv[1]) == "publish") {
    xmedia::media::ffmpeg::InstallLogBridge();
    return xmedia::RunPublish(argc - 1, argv + 1);
  }
  if (argc > 1 && std::string(argv[1]) == "record") {
    xmedia::media::ffmpeg::InstallLogBridge();
    return xmedia::RunRecord(argc - 1, argv + 1);
  }
  xmedia::Options opts;
  if (!xmedia::ParseArgs(argc, argv, &opts)) {
    return 1;
  }
  // Route FFmpeg's own logging through xmedia-kit's so a single --verbose
  // controls
  // both. Without this, libavformat's warnings go to stderr unformatted and
  // interleave with the report.
  xmedia::media::ffmpeg::InstallLogBridge();

  if (opts.subcommand == "probe") {
    return xmedia::RunProbe(opts);
  }
  if (opts.subcommand == "decode") {
    return xmedia::RunDecode(opts);
  }
  if (opts.subcommand == "sync") {
    return xmedia::RunSync(opts);
  }
  fprintf(stderr, "error: unknown subcommand '%s'\n", opts.subcommand.c_str());
  xmedia::Usage();
  return 1;
}

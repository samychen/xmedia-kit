// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "examples/html_bake_args.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits.h>  // PATH_MAX
#include <unistd.h>  // getcwd

namespace xmedia {
namespace examples {

bool ParseBakeArgs(int argc, char** argv, BakeArgs* out) {
  if (out == nullptr) {
    return false;
  }
  BakeArgs a;
  for (int i = 1; i < argc; ++i) {
    const std::string v = i + 1 < argc ? argv[i + 1] : "";
    if (std::strcmp(argv[i], "--url") == 0 && !v.empty()) {
      a.url = v;
      ++i;
    } else if (std::strcmp(argv[i], "--out") == 0 && !v.empty()) {
      a.out = v;
      ++i;
    } else if (std::strcmp(argv[i], "--seconds") == 0) {
      a.seconds = std::atof(v.c_str());
      ++i;
    } else if (std::strcmp(argv[i], "--fps") == 0) {
      a.fps = std::atoi(v.c_str());
      ++i;
    } else if (std::strcmp(argv[i], "--width") == 0) {
      a.width = std::atoi(v.c_str());
      ++i;
    } else if (std::strcmp(argv[i], "--height") == 0) {
      a.height = std::atoi(v.c_str());
      ++i;
    } else if (std::strcmp(argv[i], "--codec") == 0 && !v.empty()) {
      a.codec = v;
      ++i;
    } else if (std::strcmp(argv[i], "--clock") == 0 && !v.empty()) {
      a.clock = v;  // virtual_time | seek | live
      ++i;
    } else if (std::strcmp(argv[i], "--chunks") == 0) {
      a.chunks = std::max(1, std::atoi(v.c_str()));
      ++i;
    } else if (std::strcmp(argv[i], "--data") == 0 && !v.empty()) {
      a.data_json = v;
      ++i;
    } else if (std::strcmp(argv[i], "--lottie") == 0 && !v.empty()) {
      a.lottie_path = v;
      ++i;
    } else if (std::strcmp(argv[i], "--lottie-z") == 0) {
      a.lottie_z = std::atoi(v.c_str());
      ++i;
    } else if (std::strcmp(argv[i], "--lottie-speed") == 0) {
      a.lottie_speed = std::atof(v.c_str());
      ++i;
    } else if (std::strcmp(argv[i], "--lottie-loop") == 0) {
      a.lottie_loop = std::atoi(v.c_str());
      ++i;
    } else {
      // 未知参数显式告警（如 --hight 拼写错——静默忽略会让画布落到默认值）。
      std::fprintf(stderr, "html_bake: unknown argument '%s' (ignored)\n",
                   argv[i]);
    }
  }
  // 相对 --url → file:// 绝对 URL：CEF 需要完整 URL 才能加载，且页面内
  // 相对子资源（音频等）按 URL 目录解析——裸相对路径整页黑。
  if (!a.url.empty() && a.url.rfind("http://", 0) != 0 &&
      a.url.rfind("https://", 0) != 0 && a.url.rfind("file://", 0) != 0) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != nullptr) {
      const std::string path =
          a.url[0] == '/' ? a.url : std::string(cwd) + "/" + a.url;
      a.url = "file://" + path;
    }
  }
  *out = std::move(a);
  return true;
}

}  // namespace examples
}  // namespace xmedia

// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CefLayerSource 冒烟测试（gated：XMEDIA_HAS_CEF=0 时编译为空 main）。
//
// 架构：POST_BUILD 将本二进制组装进 .app bundle（cefsimple 式）：
//   <build>/media_cef_smoke.app/
//     Contents/MacOS/media_cef_smoke            ← 本二进制
//     Contents/Frameworks/CE.framework/         ← CEF framework 拷贝
//     Contents/Frameworks/media_cef_smoke Helper.app/
//       Contents/MacOS/media_cef_smoke Helper   ← 本二进制拷贝（子进程）
//     Contents/Resources/icudtl.dat, *.pak
//     Contents/Info.plist
//
// main() 拦截 --type=（Chromium 重启本二进制作为 renderer/GPU 子进程）
// → CefExecuteProcess。browser 进程走 RunSmoke()：创建 CefLayerSource、
// 等 OnPaint 帧进 mailbox、验证元数据采集。
//
// 不用 gtest：二进制链接 libcef 延迟符号，gtest_discover 探针在无 CEF
// 环境下崩（已实测）。

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include "media/cef/cef_app.h"
#include "media/cef/cef_layer_source.h"
#include "media/cef/cef_osr_client.h"
#include "media/cef/cef_runtime.h"

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF

#include "include/cef_app.h"
#include "include/wrapper/cef_library_loader.h"

using xmedia::media::CefLayerSource;

namespace {

std::unique_ptr<CefScopedLibraryLoader>& FrameworkLoader() {
  static std::unique_ptr<CefScopedLibraryLoader> loader;
  return loader;
}

bool LoadFrameworkForHelper() {
  if (!FrameworkLoader()) {
    FrameworkLoader() = std::make_unique<CefScopedLibraryLoader>();
  }
  return FrameworkLoader()->LoadInHelper();
}

bool DistroAvailable() {
  const char* root = std::getenv("XMEDIA_CEF_ROOT");
  if (root == nullptr || root[0] == '\0') {
    return false;
  }
  const std::string base = root;
  return std::ifstream(base + "/include/cef_version.h").good();
}

// RunSmoke 与框架装载助手同处匿名命名空间；main() 全局调用它。
int RunSmoke() {
  if (!DistroAvailable()) {
    printf("SKIP: no CEF distro (XMEDIA_CEF_ROOT unset)\n");
    return 0;
  }

  CefLayerSource::Config config;
  config.name = "html_smoke";
  config.z = 10;
  config.width = 320;
  config.height = 240;
  config.fps = 30;
  config.url = "file://" + std::string(XMEDIA_TESTDATA_DIR) + "/smoke.html";

  CefLayerSource layer;
  if (!layer.Start(config)) {
    printf("FAIL: the HTML layer failed to start\n");
    return 1;
  }
  for (int i = 0; i < 50; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!layer.elements().empty()) {
      break;
    }
  }
  bool saw_data_z = false;
  for (const auto& e : layer.elements().entries()) {
    saw_data_z = saw_data_z || e.has_z;
  }
  layer.Stop();
  if (layer.elements().empty() || !saw_data_z) {
    printf("FAIL: element metadata not collected\n");
    return 1;
  }
  printf("PASS: OnPaint frames published, element metadata collected\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // CEF 单二进制子进程模型：Chromium 以 --type=... 重启本二进制作为
  // renderer/GPU 子进程。拦截后加载 framework 并委托 CefExecuteProcess。
  for (int i = 0; i < argc; ++i) {
    if (strncmp(argv[i], "--type=", 7) == 0) {
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
      if (!LoadFrameworkForHelper()) {
        fprintf(stderr, "cef smoke: helper framework load failed\n");
        return 1;
      }
      CefMainArgs main_args(argc, argv);
      CefRefPtr<xmedia::cef::MediaCefApp> app(new xmedia::cef::MediaCefApp);
      return CefExecuteProcess(main_args, app.get(), nullptr);
#else
      return 0;
#endif
    }
  }
  // Browser process: RunSmoke() → CefLayerSource::Start → CefRuntime::Init
  // 自己装载 framework（cef_load_library 非幂等：这里预装载会让 Init 的
  // 装载调用因 g_libcef_handle 已设而返回 0，被误判为失败）。只有 helper
  // 子进程路径需要先装载（CefExecuteProcess 契约）。
  return RunSmoke();
}

#else  // !XMEDIA_HAS_CEF

int main() {
  printf("SKIP: built without XMEDIA_ENABLE_CEF\n");
  return 0;
}

#endif  // XMEDIA_HAS_CEF

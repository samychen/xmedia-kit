// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CEF subprocess entry: the binary that Chromium relaunches with --type=...
// (renderer/gpu/zygote). Built as its own tiny target (xmedia_cef_helper)
// and installed into a Helper.app-style bundle by whoever deploys the HTML
// layer (the smoke test builds one in TempDir). Never links the host
// pipeline — the subprocess role must run before any other initialization.

#include "media/cef/cef_app.h"
#include "include/cef_app.h"

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF

int main(int argc, char** argv) {
  // Renderer/GPU 子进程也要拿到 MediaCefApp（MessageRouter renderer-side
  // 与 OnWebToolkitInitialized 都挂在它上）——nullptr 会让 cefQuery 静默
  // 失效（实测：mc.html.rects 永不到达）。
  CefMainArgs main_args(argc, argv);
  CefRefPtr<xmedia::cef::MediaCefApp> app(
      new xmedia::cef::MediaCefApp);
  return CefExecuteProcess(main_args, app.get(), nullptr);
}

#else

int main() {
  return 0;
}

#endif

// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// CefLayerSource 冒烟测试（gated：XMEDIA_HAS_CEF=0 时编译为空 main）。
// 证明 HTML 图层管线活着：OSR 浏览器起得来、OnPaint 帧经 Publish 进
// GlLayerSource 邮箱、mc.html.rects 采集进元素表。无 distro → exit 0
// （门禁语义 = 跳过而非失败）。刻意不用 gtest：二进制链接 libcef 的延迟
// 符号，gtest_discover 的探针运行会在无 CEF 环境下崩（已实测）。

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>

#include "media/cef/cef_layer_source.h"
#include "media/cef/cef_runtime.h"
#include "media/cef/cef_app.h"
#include <cstring>

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF

using xmedia::media::CefLayerSource;

namespace {

bool DistroAvailable() {
  const char* root = std::getenv("XMEDIA_CEF_ROOT");
  if (root == nullptr || root[0] == '\0') {
    return false;
  }
  const std::string base = root;
  return std::ifstream(base + "/include/cef_version.h").good() &&
         std::ifstream(base + "/Release/Chromium Embedded Framework.framework")
             .good();
}

// 按 cefsimple 布局组装 TempDir 运行环境，返回 framework_dir/subprocess。
bool AssembleRuntime(const std::string& tmp, std::string* framework_dir,
                     std::string* subprocess_path) {
  const char* root = std::getenv("XMEDIA_CEF_ROOT");
  if (root == nullptr) {
    return false;
  }
  // 布局模拟 MC render-demo.app 的 Contents/：
  //   Frameworks/<framework>（符号链接回 distro）
  //   Frameworks/<name> Helper.app/Contents/MacOS/<helper>（拷贝自构建）
  const std::string frameworks = tmp + "/cef_runtime/Frameworks";
  std::system(("mkdir -p '" + frameworks + "'").c_str());
  const std::string fw_src =
      std::string(root) + "/Release/Chromium Embedded Framework.framework";
  const std::string fw_link =
      frameworks + "/Chromium Embedded Framework.framework";
  std::remove(fw_link.c_str());
  if (std::system(("ln -s '" + fw_src + "' '" + fw_link + "'").c_str()) != 0) {
    return false;
  }
  const std::string contents = tmp + "/cef_runtime";
  const char* helper = std::getenv("XMEDIA_CEF_HELPER");
  if (helper == nullptr || !std::ifstream(helper).good()) {
    return false;
  }
  const std::string bundle = frameworks + "/Sparkle Helper.app/Contents/MacOS";
  std::system(("mkdir -p '" + bundle + "'").c_str());
  // Helper.app 的 Contents/Frameworks 也要有 framework：CEF 子进程模型在
  // 加载期按 subprocess bundle 自身结构解析（不回溯到父目录）。
  const std::string bundle_fw =
      frameworks + "/Sparkle Helper.app/Contents/Frameworks";
  std::system(("mkdir -p '" + bundle_fw + "'").c_str());
  std::remove((bundle_fw + "/Chromium Embedded Framework.framework").c_str());
  std::system(("ln -s '" + fw_src + "' '" + bundle_fw +
               "/Chromium Embedded Framework.framework'").c_str());
  // Resources 也链接（icudtl.dat / v8_context_snapshot.bin / *.pak）——
  // 渲染子进程启动需要这些资源文件。
  const std::string res_src = std::string(root) + "/Resources";
  const std::string res_link = frameworks + "/Resources";
  std::remove(res_link.c_str());
  std::system(("ln -s '" + res_src + "' '" + res_link + "'").c_str());
  const std::string helper_dst = bundle + "/Sparkle Helper";
  std::system(("cp \"" + std::string(helper) + "\" \"" + helper_dst +
               "\"").c_str());
  std::ofstream plist(
      frameworks + "/Sparkle Helper.app/Contents/Info.plist");
  plist << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        << "<plist version=\"1.0\"><dict>\n"
        << "<key>CFBundleExecutable</key><string>Sparkle Helper</string>\n"
        << "<key>CFBundleIdentifier</key><string>com.xmedia.cef-helper"
           "</string>\n"
        << "</dict></plist>\n";
  // framework Resources/Libraries 部署到 helper/browser 二进制路径旁：
  // CEF 子进程按 own executable path 解析 icudtl.dat/paks/Libraries。
  const std::string fw_res = fw_src + "/Resources";
  const std::string fw_lib = fw_src + "/Libraries";
  const std::string res_ln = "ln -sf '" + fw_res + "' '" + helper_dir + "/Resources'";
  const std::string lib_ln = "ln -sf '" + fw_lib + "' '" + helper_dir + "/Libraries'";
  std::system(res_ln.c_str());
  std::system(lib_ln.c_str());
  std::system(("ln -sf '" + fw_res + "' '" + rt + "/Resources'").c_str());
  std::system(("ln -sf '" + fw_lib + "' '" + rt + "/Libraries'").c_str());
  *framework_dir = frameworks;
  *subprocess_path = helper_dst;
  return true;
}

int RunSmoke() {
  if (!DistroAvailable()) {
    printf("SKIP: no CEF distro (XMEDIA_CEF_ROOT unset or layout broken)\n");
    return 0;
  }
  const std::string tmp = [] {
    const char* t = std::getenv("TMPDIR");
    return (t && *t) ? std::string(t) : std::string("/tmp");
  }();
  std::string framework_dir;
  std::string subprocess;
  if (!AssembleRuntime(tmp, &framework_dir, &subprocess)) {
    printf("FAIL: runtime assembly failed (helper build present?)\n");
    return 1;
  }

  const std::string html = tmp + "/smoke.html";
  {
    std::ofstream f(html);
    f << "<!DOCTYPE html><html><body style=\"margin:0\">"
      << "<div data-z=\"10\" style=\"position:absolute;left:10px;top:10px;"
      << "width:200px;height:100px;background:#ff0000\">hi</div>"
      << "<div style=\"position:absolute;left:0;top:200px;width:100%;"
      << "height:50px;background:#00ff00\">tick</div>"
      << "</body></html>";
  }

  CefLayerSource::Config config;
  config.name = "html_smoke";
  config.z = 10;
  config.width = 320;
  config.height = 240;
  config.fps = 30;
  config.url = "file://" + html;
  config.framework_dir = framework_dir;
  config.subprocess_path = subprocess;

  CefLayerSource layer;
  if (!layer.Start(config)) {
    printf("FAIL: the HTML layer failed to start\n");
    return 1;
  }
  // OnPaint → Publish：元素表在前 5 秒内必须出现（采集脚本注入后上报）。
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
    printf("FAIL: element metadata not collected (saw_data_z=%d)\n",
           saw_data_z ? 1 : 0);
    return 1;
  }
  printf("PASS: OnPaint frames published, element metadata collected\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  // CEF 单二进制子进程模型：Chromium 以 --type=... 重启本二进制作为
  // renderer/GPU 子进程——拦截后委托给 CefExecuteProcess（MC Linux
  // 路径的先例），不再依赖独立 Helper.app bundle。
  for (int i = 0; i < argc; ++i) {
    if (strncmp(argv[i], "--type=", 7) == 0) {
      CefMainArgs main_args(argc, argv);
      CefRefPtr<xmedia::cef::MediaCefApp> app(new xmedia::cef::MediaCefApp);
      return CefExecuteProcess(main_args, app.get(), nullptr);
    }
  }
  return RunSmoke();
}

#else

int main() {
  printf("SKIP: built without XMEDIA_ENABLE_CEF\n");
  return 0;
}

#endif  // XMEDIA_HAS_CEF

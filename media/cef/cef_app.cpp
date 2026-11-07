// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// MediaCefApp：browser + renderer 双进程角色的 CEF App 实现。browser 侧
// 的命令行开关与 external pump 臂；renderer 侧的 MessageRouter（页面 JS
// 的 window.cefQuery / cefQueryCancel 协议）。
//
// CefArmExternalMessagePump 的实现也在这里（而非 cef_runtime.cpp）：
// helper 子进程链接 cef_app.cpp.o 但不链接 runtime——OnScheduleMessagePump
// 的符号必须在 helper 里自足。

#include "media/cef/cef_app.h"

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>

namespace xmedia {
namespace cef {
namespace {

// External message pump arming（见 MediaCefApp::OnScheduleMessagePumpWork）。
// CEF 可从任意线程回调 OnScheduleMessagePumpWork；DoMessageLoopWork 必须在
// CefInitialize 线程跑。跨线程 CefPostTask 到达不再触发回调的实测缺陷
// （MC T2 记录）由 10ms 看门狗兜底泵覆盖。
std::atomic<bool> g_cef_pump_armed{false};
std::atomic<int64_t> g_cef_pump_earliest_ms{0};
std::atomic<int64_t> g_cef_last_work_ms{0};
// Open OSR browsers (OnAfterCreated − OnBeforeClose)。Shutdown 依此 drain。
std::atomic<int> g_cef_open_browsers{0};

int64_t NowMs() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock::now().time_since_epoch())
      .count();
}

std::string MakeUniqueCefCachePath() {
  const char* tmp = std::getenv("TMPDIR");
  if (!tmp || !*tmp) {
    tmp = std::getenv("TMP");
  }
  if (!tmp || !*tmp) {
    tmp = "/tmp";
  }
  std::string path(tmp);
  if (!path.empty() && path.back() == '/') {
    path.pop_back();
  }
  path += "/mc_cef_cache_";
  path += std::to_string(static_cast<long long>(::getpid()));
  return path;
}

}  // namespace

void CefArmExternalMessagePump(int64_t delay_ms) {
  const int64_t when = delay_ms <= 0 ? int64_t{0} : (NowMs() + delay_ms);
  int64_t prev = g_cef_pump_earliest_ms.load(std::memory_order_relaxed);
  for (;;) {
    // Keep the soonest deadline (0 = ASAP).
    if (prev != 0 && (when == 0 || when > prev)) {
      break;
    }
    if (g_cef_pump_earliest_ms.compare_exchange_weak(
            prev, when, std::memory_order_relaxed)) {
      break;
    }
  }
  g_cef_pump_armed.store(true, std::memory_order_release);
}

void CefTrackOsBrowserCreated() {
  g_cef_open_browsers.fetch_add(1, std::memory_order_acq_rel);
}

void CefTrackOsBrowserClosed() {
  g_cef_open_browsers.fetch_sub(1, std::memory_order_acq_rel);
}

int CefOpenBrowserCount() {
  return g_cef_open_browsers.load(std::memory_order_acquire);
}

void CefForcePumpTick() {
  g_cef_pump_armed.store(false, std::memory_order_relaxed);
  g_cef_pump_earliest_ms.store(0, std::memory_order_relaxed);
  g_cef_last_work_ms.store(NowMs(), std::memory_order_relaxed);
  CefDoMessageLoopWork();
}

bool CefPumpShouldWork(int64_t now_ms) {
  // Armed fast path: CEF asked via OnScheduleMessagePumpWork (honor the
  // deadline). Watchdog fallback: fixed-rate pump when arm notifications
  // stop arriving (bounded to one pass per interval).
  if (g_cef_pump_armed.load(std::memory_order_acquire)) {
    const int64_t earliest =
        g_cef_pump_earliest_ms.load(std::memory_order_relaxed);
    if (earliest == 0 || now_ms >= earliest) {
      g_cef_pump_armed.store(false, std::memory_order_relaxed);
      g_cef_pump_earliest_ms.store(0, std::memory_order_relaxed);
      g_cef_last_work_ms.store(now_ms, std::memory_order_relaxed);
      return true;
    }
  }
  int64_t last = g_cef_last_work_ms.load(std::memory_order_relaxed);
  if (now_ms - last >= 10 &&
      g_cef_last_work_ms.compare_exchange_strong(last, now_ms)) {
    g_cef_pump_armed.store(false, std::memory_order_relaxed);
    g_cef_pump_earliest_ms.store(0, std::memory_order_relaxed);
    return true;
  }
  return false;
}

}  // namespace cef
}  // namespace xmedia

namespace xmedia {
namespace cef {

void MediaCefApp::OnBeforeCommandLineProcessing(
    const CefString& /*process_type*/,
    CefRefPtr<CefCommandLine> command_line) {
  if (!command_line) {
    return;
  }
  // Local CG templates load as file:// (assets/html/*.html). Chromium blocks
  // file→file XHR/fetch and some relative loads without these switches.
  // Do NOT add disable-web-security: that also weakens CORS for https://
  // overlays. Prefer scoped file-access switches.
  command_line->AppendSwitch("allow-file-access-from-files");
  command_line->AppendSwitch("allow-universal-access-from-files");
  // macOS：禁用 Chromium Safe Storage 钥匙串。OSR CG 层不需要钥匙串级
  // 持久加密（每次运行全新 profile）；ad-hoc 签名变化会使钥匙串 ACL 失
  // 配，每次启动弹"Chromium Safe Storage"密码框。use-mock-keychain 用
  // 固定密钥替代。
  command_line->AppendSwitch("use-mock-keychain");
  // OSR software rendering: GPU subprocess on macOS outside .app bundle
  // SIGSEGV (framework Resources unresolvable from bare executable).
  // OnPaint still fires via the software compositor — the CefOsFrame
  // BGRA path doesn't need GPU textures.
  command_line->AppendSwitch("disable-gpu");
  command_line->AppendSwitch("disable-gpu-compositing");
}

void MediaCefApp::OnScheduleMessagePumpWork(int64_t delay_ms) {
  CefArmExternalMessagePump(delay_ms);
}

void MediaCefApp::OnWebKitInitialized() {
  // Renderer-side router: injects window.cefQuery / cefQueryCancel.
  CefMessageRouterConfig config;
  renderer_router_ = CefMessageRouterRendererSide::Create(config);
}

void MediaCefApp::OnContextCreated(CefRefPtr<CefBrowser> browser,
                                   CefRefPtr<CefFrame> frame,
                                   CefRefPtr<CefV8Context> context) {
  if (renderer_router_) {
    renderer_router_->OnContextCreated(browser, frame, context);
  }
}

void MediaCefApp::OnContextReleased(CefRefPtr<CefBrowser> browser,
                                    CefRefPtr<CefFrame> frame,
                                    CefRefPtr<CefV8Context> context) {
  if (renderer_router_) {
    renderer_router_->OnContextReleased(browser, frame, context);
  }
}

bool MediaCefApp::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
    CefProcessId source_process, CefRefPtr<CefProcessMessage> message) {
  if (!renderer_router_) {
    return false;
  }
  return renderer_router_->OnProcessMessageReceived(browser, frame,
                                                    source_process, message);
}

}  // namespace cef
}  // namespace xmedia

#else  // XMEDIA_HAS_CEF

namespace xmedia {
namespace cef {

// 非 CEF 构建的 no-op：编译门保证这些 TU 只在 HAS_CEF=1 时被链接，但
// helper 子进程 target 无条件编译 cef_app.cpp（含 #else 桩），保持入口
// 符号存在。
void CefArmExternalMessagePump(int64_t) {}
void CefTrackOsBrowserCreated() {}
void CefTrackOsBrowserClosed() {}
int CefOpenBrowserCount() {
  return 0;
}
void CefForcePumpTick() {}
bool CefPumpShouldWork(int64_t) {
  return false;
}

}  // namespace cef
}  // namespace xmedia

#endif  // XMEDIA_HAS_CEF

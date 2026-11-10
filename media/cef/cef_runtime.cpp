#include "base/logging.h"
#include "cef_runtime.h"

#include <iostream>

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF

#include "media/cef/cef_app.h"
#include "media/cef/cef_osr_client.h"
#include "media/cef/cef_platform.h"

#include "include/cef_app.h"
#include "include/cef_browser.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <unistd.h>

#endif  // XMEDIA_HAS_CEF

namespace xmedia {
namespace cef {

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF

namespace {

// External message pump arming (see CefBrowserProcessHandler).
// CEF may call OnScheduleMessagePumpWork from any thread; DoMessageLoopWork
// must run on the CefInitialize thread (RenderEngine main EventLoop). Calling
// CefDoMessageLoopWork from the Graphics thread breaks input (SendKeyEvent).
// 就强制泵一次（无队列时空转开销可忽略）。Linux 同样受益，无平台分支。

// Open OSR browsers (OnAfterCreated − OnBeforeClose). Shutdown must drain to 0
// via CefDoMessageLoopWork before CefShutdown when external_message_pump=true.

int64_t NowMs() {
  using clock = std::chrono::steady_clock;
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             clock::now().time_since_epoch())
      .count();
}

std::string MakeUniqueCefCachePath() {
  // Prefer process-private dir under TMPDIR/gettmpdir so concurrent demos do
  // not share a singleton /tmp/mc_cef_cache root.
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




#endif  // XMEDIA_HAS_CEF

bool CefBundlePaths(CefDeployPaths* out) {
  if (!out) {
    return false;
  }
  out->framework_dir.clear();
  out->subprocess_path.clear();

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
  // Per-platform deployment layout: macOS .app bundle (cefsimple style) vs
  // Linux flat distro layout — see cef_platform_mac.cc / cef_platform_linux.cc.
  return CefPlatformDeployPaths(out);
#else
  return false;
#endif
}

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF

struct CefOsBrowserHandle {
  CefRefPtr<CefOsClient> client;
};

namespace {

bool LoadCefFramework(const std::string& framework_dir) {
  // macOS: dlopen Chromium Embedded Framework.framework (sandbox model).
  // Linux: libcef.so linked at build time — nothing to load.
  // See cef_platform_mac.cc / cef_platform_linux.cc.
  return CefPlatformLoadFramework(framework_dir);
}

}  // namespace

#endif  // XMEDIA_HAS_CEF

CefRuntime& CefRuntime::Instance() {
  static CefRuntime instance;
  return instance;
}

bool CefRuntime::Init(const std::string& framework_dir,
                      const std::string& subprocess_path) {
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
  if (available_) {
    return true;
  }

  if (!library_loaded_) {
    if (!LoadCefFramework(framework_dir)) {
      available_ = false;
      return false;
    }
    library_loaded_ = true;
  }

  CefMainArgs main_args;
  {
    // Browser-process main args per platform: macOS _NSGetArgc/_NSGetArgv,
    // Linux /proc/self/cmdline (see CefPlatformBrowserMainArgs). Chromium
    // needs the real command line (switches, logging) — fall back to an
    // empty CefMainArgs only if resolution fails.
    int argc = 0;
    char** argv = nullptr;
    if (CefPlatformBrowserMainArgs(&argc, &argv)) {
      main_args = CefMainArgs(argc, argv);
    }
  }

  CefSettings settings;
  settings.windowless_rendering_enabled = true;
  // GLFW host: pump via CefDoMessageLoopWork on the **main** EventLoop
  // (same thread as CefInitialize). Graphics must not call it.
  settings.external_message_pump = true;
  settings.no_sandbox = true;

  // Avoid default cache singleton clashes when re-running demos concurrently.
  // Per-process path under TMPDIR (fallback /tmp), not a shared mc_cef_cache.
  CefString(&settings.root_cache_path) = MakeUniqueCefCachePath();

  if (!framework_dir.empty()) {
#if defined(OS_MAC)
    // macOS: directory that contains Chromium Embedded Framework.framework.
    const std::string fw =
        framework_dir + "/Chromium Embedded Framework.framework";
    CefString(&settings.framework_dir_path) = fw;
    // Explicit resource paths: subprocesses (GPU/renderer) resolve paks
    // relative to the framework bundle's Resources dir. Without these,
    // the subprocess looks for paks relative to its own binary path and
    // crashes when the layout doesn't match the host app bundle.
    const std::string res_dir = fw + "/Resources";
    CefString(&settings.resources_dir_path) = res_dir;
    CefString(&settings.locales_dir_path) = res_dir + "/locales";
#else
    // Linux: framework_dir already IS the directory containing libcef.so
    // (flat distro layout); the field is macOS-focused, pass it through.
    CefString(&settings.framework_dir_path) = framework_dir;
#endif
  }
  if (!subprocess_path.empty()) {
    // macOS: Helper bundle executable. Linux: this executable itself
    // (single-binary subprocess; main() delegates via CefPlatformSubprocessMain).
    CefString(&settings.browser_subprocess_path) = subprocess_path;
  }

  CefRefPtr<MediaCefApp> app(new MediaCefApp);
  if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
    LOG(ERROR) << "[CefRuntime] CefInitialize failed (exit_code="
              << CefGetExitCode()
              << "). Check helper executable and framework paths.\n";
    available_ = false;
    return false;
  }

  available_ = true;
  return true;
#else
  (void)framework_dir;
  (void)subprocess_path;
  available_ = false;
  return false;
#endif
}

void CefRuntime::DoMessageLoopWork() {
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
  if (!available_) {
    return;
  }
  const int64_t now_ms = NowMs();
  if (CefPumpShouldWork(now_ms)) {
    CefDoMessageLoopWork();
  }
#endif
}

void CefRuntime::DoMessageLoopWorkForced() {
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
  if (!available_) {
    return;
  }
  CefForcePumpTick();
#endif
}

void CefRuntime::Shutdown() {
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
  if (!available_) {
    return;
  }
  // Caller must CloseBrowser all OSR browsers first (engine StopAll).
  // external_message_pump: CloseBrowser posts TID_UI work — must pump until
  // OnBeforeClose (open count → 0) before CefShutdown, or exit can crash/UB.
  constexpr int kDrainTimeoutMs = 2000;
  const int64_t deadline = NowMs() + kDrainTimeoutMs;
  int open = CefOpenBrowserCount();
  while (open > 0 && NowMs() < deadline) {
    CefArmExternalMessagePump(0);
    CefForcePumpTick();
    open = CefOpenBrowserCount();
    if (open > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  if (open > 0) {
    LOG(ERROR) << "[CefRuntime] Shutdown: " << open
              << " browser(s) still open after " << kDrainTimeoutMs
              << "ms drain; proceeding to CefShutdown\n";
  }
  CefShutdown();
#endif
  available_ = false;
}

BrowserHandle CefRuntime::CreateOsBrowser(const std::string& url, int width,
                                          int height, bool transparent, int fps,
                                          std::shared_ptr<CefOsFrame> frame,
                                          HtmlBrowserListener* listener,
                                          bool external_begin_frame) {
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
  if (!available_ || !frame || width <= 0 || height <= 0) {
    return nullptr;
  }

  CefRefPtr<CefOsClient> client(new CefOsClient(width, height, frame));
  client->SetHtmlBrowserListener(listener);

  CefWindowInfo window_info;
#if defined(OS_MAC)
  window_info.SetAsWindowless(kNullWindowHandle);
#elif defined(OS_WIN)
  window_info.SetAsWindowless(nullptr);
#else
  window_info.SetAsWindowless(kNullWindowHandle);
#endif
  window_info.shared_texture_enabled = false;
  // Bake VirtualTime: host drives paints via SendExternalBeginFrame.
  window_info.external_begin_frame_enabled = external_begin_frame;

  CefBrowserSettings browser_settings;
  // html_fps from scene config; CEF windowless_frame_rate is a paint cap.
  browser_settings.windowless_frame_rate =
      std::max(1, std::min(fps > 0 ? fps : 30, 60));
  if (transparent) {
    // Alpha 0 → transparent OSR background (CEF default is already
    // transparent when windowless; keep explicit for clarity).
    browser_settings.background_color = CefColorSetARGB(0, 0, 0, 0);
  } else {
    browser_settings.background_color = CefColorSetARGB(255, 255, 255, 255);
  }

  // Sync create on the CEF UI thread (main/NSRunLoop on macOS). Async
  // CreateBrowser was returning true but never delivering OnAfterCreated
  // under our GLFW host; Sync surfaces failures immediately.
  CefRefPtr<CefBrowser> browser = CefBrowserHost::CreateBrowserSync(
      window_info, client, url, browser_settings, nullptr, nullptr);
  if (!browser) {
    LOG(ERROR) << "[CefRuntime] CreateBrowserSync failed for url=" << url
              << "\n";
    return nullptr;
  }

  if (external_begin_frame) {
    client->EnableVirtualTime(fps > 0 ? fps : 30);
    // Kick VT while loading so OnLoadEnd / first paint can progress under
    // external_begin_frame (otherwise renderer may stall with no BeginFrame).
    client->RequestVtStep();
  }

  // Flush UI tasks so first navigation/paint can start before the host's
  // first RenderFrame (StartAll runs during Init, before the GLFW loop).
  for (int i = 0; i < 50; ++i) {
    DoMessageLoopWorkForced();
    {
      std::lock_guard<std::mutex> lock(frame->mutex);
      if (frame->dirty && frame->width > 0 && frame->height > 0) {
        break;
      }
    }
    if (external_begin_frame && (i % 5) == 4) {
      client->RequestVtStep();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  auto handle = std::make_shared<CefOsBrowserHandle>();
  handle->client = client;
  return handle;
#else
  (void)url;
  (void)width;
  (void)height;
  (void)transparent;
  (void)fps;
  (void)frame;
  (void)listener;
  (void)external_begin_frame;
  return nullptr;
#endif
}

void CefRuntime::RequestVtStep(BrowserHandle handle) {
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
  if (!handle || !handle->client) {
    return;
  }
  handle->client->RequestVtStep();
#else
  (void)handle;
#endif
}

void CefRuntime::Invalidate(BrowserHandle handle) {
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
  if (!handle || !handle->client) {
    return;
  }
  handle->client->InvalidateView();
#else
  (void)handle;
#endif
}

void CefRuntime::CloseBrowser(BrowserHandle handle) {
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
  if (!handle || !handle->client) {
    return;
  }
  handle->client->SetHtmlBrowserListener(nullptr);
  // Sets pending_close_ if CreateBrowser has not finished; closes now if ready.
  handle->client->RequestClose();
#else
  (void)handle;
#endif
}

void CefRuntime::LoadUrl(BrowserHandle handle, const std::string& url) {
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
  if (!handle || !handle->client) {
    return;
  }
  handle->client->QueueLoadUrl(url);
#else
  (void)handle;
  (void)url;
#endif
}

void CefRuntime::ExecuteJavaScript(BrowserHandle handle,
                                   const std::string& code) {
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
  if (!handle || !handle->client) {
    return;
  }
  handle->client->QueueExecuteJavaScript(code);
#else
  (void)handle;
  (void)code;
#endif
}

void CefRuntime::SendMouseClick(BrowserHandle handle, int x, int y, int button,
                                bool mouse_up, uint32_t modifiers) {
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
  if (!handle || !handle->client) {
    return;
  }
  handle->client->QueueSendMouseClick(x, y, button, mouse_up, modifiers);
#else
  (void)handle;
  (void)x;
  (void)y;
  (void)button;
  (void)mouse_up;
  (void)modifiers;
#endif
}

void CefRuntime::SendMouseMove(BrowserHandle handle, int x, int y,
                               bool mouse_leave, uint32_t modifiers) {
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
  if (!handle || !handle->client) {
    return;
  }
  handle->client->QueueSendMouseMove(x, y, mouse_leave, modifiers);
#else
  (void)handle;
  (void)x;
  (void)y;
  (void)mouse_leave;
  (void)modifiers;
#endif
}

void CefRuntime::SendMouseWheel(BrowserHandle handle, int x, int y, int delta_x,
                                int delta_y, uint32_t modifiers) {
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
  if (!handle || !handle->client) {
    return;
  }
  handle->client->QueueSendMouseWheel(x, y, delta_x, delta_y, modifiers);
#else
  (void)handle;
  (void)x;
  (void)y;
  (void)delta_x;
  (void)delta_y;
  (void)modifiers;
#endif
}

void CefRuntime::SendKeyEvent(BrowserHandle handle, int windows_key_code,
                              int native_key_code, bool key_up, bool is_char,
                              uint32_t character, uint32_t unmodified_character,
                              uint32_t modifiers) {
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
  if (!handle || !handle->client) {
    return;
  }
  handle->client->QueueSendKeyEvent(windows_key_code, native_key_code, key_up,
                                    is_char, character, unmodified_character,
                                    modifiers);
#else
  (void)handle;
  (void)windows_key_code;
  (void)native_key_code;
  (void)key_up;
  (void)is_char;
  (void)character;
  (void)unmodified_character;
  (void)modifiers;
#endif
}

void CefRuntime::SetFocus(BrowserHandle handle, bool focus) {
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
  if (!handle || !handle->client) {
    return;
  }
  handle->client->QueueSetFocus(focus);
#else
  (void)handle;
  (void)focus;
#endif
}

void CefRuntime::ImeCommitText(BrowserHandle handle,
                               const std::string& utf8_text) {
#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
  if (!handle || !handle->client || utf8_text.empty()) {
    return;
  }
  handle->client->QueueImeCommitText(utf8_text);
#else
  (void)handle;
  (void)utf8_text;
#endif
}

}  // namespace cef
}  // namespace xmedia

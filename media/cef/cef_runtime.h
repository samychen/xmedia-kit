#pragma once

// CefRuntime — process-wide CEF lifecycle + OSR browser factory for HTML CG.
//
// Compile gate:
// - CMake sets XMEDIA_HAS_CEF=1 when ENABLE_CEF + CEF_ROOT are valid.
// - framework/render/cef/*.cpp are linked into opengl_render only then.
// - Callers (HtmlLayerProducer / RenderEngine, Tasks 4–5) must guard with
//   `#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF`.
//
// Threading contract:
// - Init / Shutdown / DoMessageLoopWork: call from the engine **main** thread
//   (same thread as CefInitialize). Graphics thread must not pump CEF.
// - CreateOsBrowser / CloseBrowser / LoadUrl / ExecuteJavaScript: may be called
//   from producer Start/Stop or control paths; CEF work is posted to TID_UI
//   where required.
// - OSR OnPaint (CefOsClient): CEF UI thread → writes CefOsFrame under mutex.
// - ProcessGpu (Task 4): Graphics thread → locks CefOsFrame, converts
//   BGRA→RGBA, UpsertRgbaTexture. Never touch GL from CEF callbacks.
//
// BGRA→RGBA: deferred to HtmlLayerProducer::ProcessGpu. OnPaint memcpy under
// mutex only; ProcessGpu copies/swaps under lock then converts unlocked.
//
// macOS deploy layout (Task 3 POST_BUILD, beside the host executable):
//   <exe-dir>/Chromium Embedded Framework.framework/
//   <App>.app/Contents/Frameworks/<basename> Helper.app/  (+ GPU/…)
//   <App>.app/Contents/Frameworks/Chromium Embedded Framework.framework
// Use CefBundlePaths() then Init(framework_dir, subprocess_path). RenderEngine
// Init wiring is Task 5 — callers may probe paths earlier for smoke tests.

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace xmedia {
namespace cef {

// Shared CPU paint buffer between CEF UI (writer) and Graphics (reader).
// Lock mutex for any read/write of width/height/bgra/dirty.
struct CefOsFrame {
  std::mutex mutex;
  int width = 0;
  int height = 0;
  std::vector<uint8_t> bgra;  // tightly packed BGRA8 (CEF OnPaint order)
  bool dirty = false;
};

// JS → C++ MessageRouter handshake (mc.html.*). Implemented by
// HtmlLayerProducer; invoked from CefOsClient::OnQuery on the CEF UI thread.
// Ready-gate / bake consume these in later tasks — Task 1 only delivers them.
// T2 phase 1 adds mc.html.rects:<json> (per-element DOM rect + data-start /
// data-duration / data-z declarations, see html_element_meta.h); default
// body keeps listeners that don't care about element metadata compiling.
struct HtmlBrowserListener {
  virtual void OnHtmlReady() = 0;
  virtual void OnHtmlError(const std::string& message) = 0;
  virtual void OnHtmlSeekDone(int64_t t_ms) = 0;
  virtual void OnHtmlRects(const std::string& payload_json) { (void)0; }
  virtual ~HtmlBrowserListener() = default;
};

// Opaque browser handle for HtmlLayerProducer (Task 4). Empty shared_ptr =
// invalid / CEF unavailable.
struct CefOsBrowserHandle;
using BrowserHandle = std::shared_ptr<CefOsBrowserHandle>;

// Absolute paths for CefRuntime::Init derived from the running executable
// (per-platform impl in cef_platform_mac.cc / cef_platform_linux.cc).
// On macOS: framework_dir = Contents/Frameworks; subprocess_path = sibling
// Helper bundle executable named "<basename> Helper".
// On Linux: framework_dir = directory containing libcef.so (flat distro
// layout beside the exe, or $MC_CEF_RUNTIME_DIR); subprocess_path = the
// executable itself (single-binary subprocess, --type= relaunch).
// Returns false if paths cannot be resolved (deploy layout missing).
struct CefDeployPaths {
  std::string framework_dir;
  std::string subprocess_path;
};
bool CefBundlePaths(CefDeployPaths* out);

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF
// OSR lifespan counters for Shutdown drain (called from CefOsClient).
void CefTrackOsBrowserCreated();
void CefTrackOsBrowserClosed();
#endif

class CefRuntime {
 public:
  static CefRuntime& Instance();

  // framework_dir: parent dir of Chromium Embedded Framework.framework
  // subprocess_path: absolute path to CEF helper executable (Task 3)
  bool Init(const std::string& framework_dir,
            const std::string& subprocess_path);
  // external_message_pump: call only on the thread that ran CefInitialize
  // (RenderEngine main EventLoop). Do not pump from the Graphics thread.
  void DoMessageLoopWork();
  // Drain immediately (ignore arm delay). Use after GLFW input posts CEF work.
  void DoMessageLoopWorkForced();
  // CloseBrowser all OSR browsers first, then Shutdown drains OnBeforeClose
  // via the external message pump before CefShutdown.
  void Shutdown();

  bool available() const { return available_; }

  // fps → CefBrowserSettings.windowless_frame_rate (clamped to 1–60).
  // listener: optional mc.html.* MessageRouter callbacks (non-owning; clear
  // via CloseBrowser / Stop before destroying).
  // external_begin_frame: bake+virtual_time only (CefWindowInfo + DevTools VT).
  BrowserHandle CreateOsBrowser(const std::string& url, int width, int height,
                                bool transparent, int fps,
                                std::shared_ptr<CefOsFrame> frame,
                                HtmlBrowserListener* listener = nullptr,
                                bool external_begin_frame = false);
  void CloseBrowser(BrowserHandle handle);
  void LoadUrl(BrowserHandle handle, const std::string& url);
  void ExecuteJavaScript(BrowserHandle handle, const std::string& code);
  // Bake VirtualTime: +1 credit; CefOsClient posts ArmVt on TID_UI.
  void RequestVtStep(BrowserHandle handle);
  // OSR input forwarding (coords relative to browser view, top-left origin).
  void SendMouseClick(BrowserHandle handle, int x, int y, int button,
                      bool mouse_up, uint32_t modifiers);
  void SendMouseMove(BrowserHandle handle, int x, int y, bool mouse_leave,
                     uint32_t modifiers);
  void SendMouseWheel(BrowserHandle handle, int x, int y, int delta_x,
                      int delta_y, uint32_t modifiers);
  void SendKeyEvent(BrowserHandle handle, int windows_key_code,
                    int native_key_code, bool key_up, bool is_char,
                    uint32_t character, uint32_t unmodified_character,
                    uint32_t modifiers);
  void ImeCommitText(BrowserHandle handle, const std::string& utf8_text);
  void SetFocus(BrowserHandle handle, bool focus);

 private:
  CefRuntime() = default;
  CefRuntime(const CefRuntime&) = delete;
  CefRuntime& operator=(const CefRuntime&) = delete;

  bool available_ = false;
  bool library_loaded_ = false;
};

}  // namespace cef
}  // namespace xmedia

#pragma once

// CefApp for HTML CG OSR — browser + renderer process.
//
// Compile gate: this header is only meaningful when XMEDIA_HAS_CEF=1
// (CMake ENABLE_CEF + CEF_ROOT). Sources under framework/render/cef are linked
// into opengl_render only in that case; keep the #if so accidental includes
// without the define stay inert.
//
// Browser process: CefBrowserProcessHandler (command-line switches + external
// message pump arm via CefArmExternalMessagePump).
// Renderer / Helper: CefRenderProcessHandler + CefMessageRouterRendererSide so
// page JS gets window.cefQuery / cefQueryCancel (mc.html.* protocol).
//
// Threading: CefApp / process-handler callbacks run on CEF threads
// (typically TID_UI for browser-process handlers). Do not touch GL here.
// OnScheduleMessagePumpWork arms CefRuntime::DoMessageLoopWork (external pump)
// in the browser process only; the Helper links a no-op stub.

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF

#include "include/cef_app.h"
#include "include/cef_browser_process_handler.h"
#include "include/cef_render_process_handler.h"
#include "include/wrapper/cef_message_router.h"

namespace xmedia {
namespace cef {

// Called from MediaCefApp::OnScheduleMessagePumpWork (any CEF thread).
// Real impl: cef_runtime.cpp. Helper subprocess: no-op stub.
void CefArmExternalMessagePump(int64_t delay_ms);
// Pump scheduler (cef_runtime.cpp's DoMessageLoopWork calls this): returns
// true when CefDoMessageLoopWork should run now — armed with the deadline
// met, or the watchdog interval elapsed — and clears the arm state.
bool CefPumpShouldWork(int64_t now_ms);
// Shutdown drain / pump 线程轮询使用（cef_runtime.cpp）。
int CefOpenBrowserCount();
void CefForcePumpTick();

class MediaCefApp : public CefApp,
                    public CefBrowserProcessHandler,
                    public CefRenderProcessHandler {
 public:
  MediaCefApp() = default;

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }
  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
    return this;
  }

  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override;

  void OnScheduleMessagePumpWork(int64_t delay_ms) override;

  // CefRenderProcessHandler — renderer / Helper only.
  void OnWebKitInitialized() override;
  void OnContextCreated(CefRefPtr<CefBrowser> browser,
                        CefRefPtr<CefFrame> frame,
                        CefRefPtr<CefV8Context> context) override;
  void OnContextReleased(CefRefPtr<CefBrowser> browser,
                         CefRefPtr<CefFrame> frame,
                         CefRefPtr<CefV8Context> context) override;
  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

 private:
  CefRefPtr<CefMessageRouterRendererSide> renderer_router_;
  IMPLEMENT_REFCOUNTING(MediaCefApp);
};

}  // namespace cef
}  // namespace xmedia

#endif  // XMEDIA_HAS_CEF

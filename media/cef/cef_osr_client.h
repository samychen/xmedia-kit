#pragma once

// OSR CefClient + CefRenderHandler: CEF paint → shared CefOsFrame.
//
// Thread boundaries (critical):
// - GetViewRect / OnPaint run on the CEF UI thread (see CefRenderHandler).
// - They MUST only write the CPU frame buffer under CefOsFrame::mutex.
// - NEVER call OpenGL / Graphics-thread APIs from OnPaint.
// - Graphics / HtmlLayerProducer::ProcessGpu locks briefly to copy/swap
//   bgra[], converts BGRA→RGBA unlocked, then UpsertRgbaTexture.
//
// BGRA→RGBA: NOT converted here. CefOsFrame stores tightly packed BGRA8 as
// produced by CEF OnPaint (memcpy under lock only).
//
// CreateBrowser is async: browser_ is null until OnAfterCreated. Close /
// LoadUrl / ExecuteJavaScript before that are recorded under mutex_ and
// applied when the browser appears (pending_close_ closes immediately).
//
// MessageRouter (browser side): CefMessageRouterBrowserSide + Handler::OnQuery
// for mc.html.ready / mc.html.error:<msg> / mc.html.seekDone:<t_ms> /
// mc.html.rects:<json> (T2 phase 1 element metadata; raw payload forwarded to
// HtmlBrowserListener — parse/merge lives in HtmlLayerProducer +
// html_element_meta.{h,cpp}). Callbacks go to HtmlBrowserListener* (defined in
// cef_runtime.h; clear before close).
//
// VirtualTime (bake+html_bake_clock=virtual_time): external_begin_frame +
// DevTools Emulation.setVirtualTimePolicy; RequestVtStep adds credit (any
// thread); ArmVt / BeginFrame / credit consume on CEF UI only.

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF

#include "cef_runtime.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "include/cef_client.h"
#include "include/cef_devtools_message_observer.h"
#include "include/cef_life_span_handler.h"
#include "include/cef_load_handler.h"
#include "include/cef_registration.h"
#include "include/cef_render_handler.h"
#include "include/cef_request_handler.h"
#include "include/wrapper/cef_message_router.h"

namespace xmedia {
namespace cef {

// The rects-collection JS. Hosts inject it once after first paint via
// ExecuteJavaScript; the collector self-reports t=0 at inject.
std::string MakeRectsScript();

class CefOsClient : public CefClient,
                    public CefRenderHandler,
                    public CefLifeSpanHandler,
                    public CefLoadHandler,
                    public CefRequestHandler,
                    public CefMessageRouterBrowserSide::Handler,
                    public CefDevToolsMessageObserver {
 public:
  CefOsClient(int width, int height, std::shared_ptr<CefOsFrame> frame);

  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefRequestHandler> GetRequestHandler() override { return this; }

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

  // CefRenderHandler — CEF UI thread only.
  void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
  bool GetScreenInfo(CefRefPtr<CefBrowser> browser,
                     CefScreenInfo& screen_info) override;
  void OnPaint(CefRefPtr<CefBrowser> browser,
               PaintElementType type,
               const RectList& dirtyRects,
               const void* buffer,
               int width,
               int height) override;

  // CefLifeSpanHandler — CEF UI thread.
  // OnAfterCreated / OnBeforeClose also track open-browser count for
  // CefRuntime::Shutdown drain (external_message_pump).
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  // CefLoadHandler — CEF UI thread.
  void OnLoadEnd(CefRefPtr<CefBrowser> browser,
                 CefRefPtr<CefFrame> frame,
                 int httpStatusCode) override;
  void OnLoadError(CefRefPtr<CefBrowser> browser,
                   CefRefPtr<CefFrame> frame,
                   ErrorCode errorCode,
                   const CefString& errorText,
                   const CefString& failedUrl) override;

  // CefRequestHandler — MessageRouter needs OnBeforeBrowse.
  bool OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                      CefRefPtr<CefFrame> frame,
                      CefRefPtr<CefRequest> request,
                      bool user_gesture,
                      bool is_redirect) override;
  void OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                 TerminationStatus status,
                                 int error_code,
                                 const CefString& error_string) override;

  // CefMessageRouterBrowserSide::Handler — CEF UI thread.
  // Request convention (string prefix, YAGNI vs JSON):
  //   "mc.html.ready"
  //   "mc.html.error:<msg>"
  //   "mc.html.seekDone:<t_ms>"
  //   "mc.html.rects:<json>" — T2 element metadata, raw payload forwarded.
  bool OnQuery(CefRefPtr<CefBrowser> browser,
               CefRefPtr<CefFrame> frame,
               int64_t query_id,
               const CefString& request,
               bool persistent,
               CefRefPtr<Callback> callback) override;

  // CefDevToolsMessageObserver — CEF UI thread (VT bake only).
  bool OnDevToolsMessage(CefRefPtr<CefBrowser> browser,
                         const void* message,
                         size_t message_size) override;

  CefRefPtr<CefBrowser> browser() const;

  // Non-owning. Call with nullptr before destroying the listener (Stop).
  void SetHtmlBrowserListener(HtmlBrowserListener* listener);

  // Bake VirtualTime: register DevTools observer (call after browser exists).
  void EnableVirtualTime(int fps);
  // Any thread: +1 credit and post TryConsumeVtCredit to TID_UI.
  void RequestVtStep();
  // CEF UI: if credit and idle/pause and load finished → setVirtualTimePolicy.
  void TryConsumeVtCredit();

  // May be called before OnAfterCreated (any thread). Safe vs async create.
  void RequestClose();
  void QueueLoadUrl(std::string url);
  void QueueExecuteJavaScript(std::string code);
  // OSR input → CefBrowserHost (posted to TID_UI). No-op until browser exists.
  void QueueSendMouseClick(int x, int y, int button, bool mouse_up,
                           uint32_t modifiers);
  void QueueSendMouseMove(int x, int y, bool mouse_leave, uint32_t modifiers);
  void QueueSendMouseWheel(int x, int y, int delta_x, int delta_y,
                           uint32_t modifiers);
  void QueueSendKeyEvent(int windows_key_code, int native_key_code,
                         bool key_up, bool is_char, uint32_t character,
                         uint32_t unmodified_character, uint32_t modifiers);
  // macOS OSR text path: insert UTF-16 into the focused editable (reliable vs
  // synthetic key events without NSTextInputClient).
  void QueueImeCommitText(std::string utf8_text);
  void QueueSetFocus(bool focus);

  void SetViewSize(int width, int height);

 private:
  enum class VtState { Idle, Arming, WaitBudget, WaitPaint, Pause };

  void ArmVirtualTimePolicy();
  void OnVirtualTimeBudgetExpired();
  void OnVtPaintReceived();

  int view_width_ = 0;
  int view_height_ = 0;
  std::shared_ptr<CefOsFrame> frame_;

  mutable std::mutex mutex_;
  CefRefPtr<CefBrowser> browser_;
  bool pending_close_ = false;
  bool has_pending_url_ = false;
  std::string pending_url_;
  bool has_pending_js_ = false;
  std::string pending_js_;
  HtmlBrowserListener* listener_ = nullptr;

  CefRefPtr<CefMessageRouterBrowserSide> message_router_;

  // VirtualTime bake (CEF UI owns state except vt_credit_).
  bool vt_enabled_ = false;
  int vt_fps_ = 30;
  std::atomic<int> vt_credit_{0};
  VtState vt_state_ = VtState::Idle;
  int64_t chrome_vt_ms_ = 0;
  CefRefPtr<CefRegistration> vt_devtools_reg_;
  bool vt_load_finished_ = false;
  bool vt_warned_fail_ = false;

  IMPLEMENT_REFCOUNTING(CefOsClient);
};

}  // namespace cef
}  // namespace xmedia

#endif  // XMEDIA_HAS_CEF

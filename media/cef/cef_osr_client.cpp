#include "base/logging.h"
#include "cef_osr_client.h"

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <utility>

#include "include/cef_browser.h"
#include "include/cef_parser.h"
#include "include/cef_task.h"
#include "include/cef_values.h"

namespace xmedia {
namespace cef {
namespace {

class CloseBrowserTask : public CefTask {
 public:
  explicit CloseBrowserTask(CefRefPtr<CefBrowser> browser)
      : browser_(std::move(browser)) {}

  void Execute() override {
    if (browser_) {
      browser_->GetHost()->CloseBrowser(/*force_close=*/true);
    }
  }

 private:
  CefRefPtr<CefBrowser> browser_;
  IMPLEMENT_REFCOUNTING(CloseBrowserTask);
};

class NavigateTask : public CefTask {
 public:
  NavigateTask(CefRefPtr<CefBrowser> browser, std::string url)
      : browser_(std::move(browser)), url_(std::move(url)) {}

  void Execute() override {
    if (!browser_) {
      return;
    }
    CefRefPtr<CefFrame> frame = browser_->GetMainFrame();
    if (frame) {
      frame->LoadURL(url_);
    }
  }

 private:
  CefRefPtr<CefBrowser> browser_;
  std::string url_;
  IMPLEMENT_REFCOUNTING(NavigateTask);
};

class ExecuteJsTask : public CefTask {
 public:
  ExecuteJsTask(CefRefPtr<CefBrowser> browser, std::string code)
      : browser_(std::move(browser)), code_(std::move(code)) {}

  void Execute() override {
    if (!browser_) {
      return;
    }
    CefRefPtr<CefFrame> frame = browser_->GetMainFrame();
    if (frame) {
      frame->ExecuteJavaScript(code_, frame->GetURL(), 0);
    }
  }

 private:
  CefRefPtr<CefBrowser> browser_;
  std::string code_;
  IMPLEMENT_REFCOUNTING(ExecuteJsTask);
};

cef_mouse_button_type_t ToCefMouseButton(int button) {
  switch (button) {
    case 1:
      return MBT_MIDDLE;
    case 2:
      return MBT_RIGHT;
    default:
      return MBT_LEFT;
  }
}

class MouseClickTask : public CefTask {
 public:
  MouseClickTask(CefRefPtr<CefBrowser> browser, int x, int y, int button,
                 bool mouse_up, uint32_t modifiers)
      : browser_(std::move(browser)),
        x_(x),
        y_(y),
        button_(button),
        mouse_up_(mouse_up),
        modifiers_(modifiers) {}

  void Execute() override {
    if (!browser_) {
      return;
    }
    CefMouseEvent ev;
    ev.x = x_;
    ev.y = y_;
    ev.modifiers = modifiers_;
    browser_->GetHost()->SendMouseClickEvent(ev, ToCefMouseButton(button_),
                                             mouse_up_, /*clickCount=*/1);
  }

 private:
  CefRefPtr<CefBrowser> browser_;
  int x_;
  int y_;
  int button_;
  bool mouse_up_;
  uint32_t modifiers_;
  IMPLEMENT_REFCOUNTING(MouseClickTask);
};

class MouseMoveTask : public CefTask {
 public:
  MouseMoveTask(CefRefPtr<CefBrowser> browser, int x, int y, bool mouse_leave,
                uint32_t modifiers)
      : browser_(std::move(browser)),
        x_(x),
        y_(y),
        mouse_leave_(mouse_leave),
        modifiers_(modifiers) {}

  void Execute() override {
    if (!browser_) {
      return;
    }
    CefMouseEvent ev;
    ev.x = x_;
    ev.y = y_;
    ev.modifiers = modifiers_;
    browser_->GetHost()->SendMouseMoveEvent(ev, mouse_leave_);
  }

 private:
  CefRefPtr<CefBrowser> browser_;
  int x_;
  int y_;
  bool mouse_leave_;
  uint32_t modifiers_;
  IMPLEMENT_REFCOUNTING(MouseMoveTask);
};

class MouseWheelTask : public CefTask {
 public:
  MouseWheelTask(CefRefPtr<CefBrowser> browser, int x, int y, int delta_x,
                 int delta_y, uint32_t modifiers)
      : browser_(std::move(browser)),
        x_(x),
        y_(y),
        delta_x_(delta_x),
        delta_y_(delta_y),
        modifiers_(modifiers) {}

  void Execute() override {
    if (!browser_) {
      return;
    }
    CefMouseEvent ev;
    ev.x = x_;
    ev.y = y_;
    ev.modifiers = modifiers_;
    browser_->GetHost()->SendMouseWheelEvent(ev, delta_x_, delta_y_);
  }

 private:
  CefRefPtr<CefBrowser> browser_;
  int x_;
  int y_;
  int delta_x_;
  int delta_y_;
  uint32_t modifiers_;
  IMPLEMENT_REFCOUNTING(MouseWheelTask);
};

class KeyEventTask : public CefTask {
 public:
  KeyEventTask(CefRefPtr<CefBrowser> browser, int windows_key_code,
               int native_key_code, bool key_up, bool is_char,
               uint32_t character, uint32_t unmodified_character,
               uint32_t modifiers)
      : browser_(std::move(browser)),
        windows_key_code_(windows_key_code),
        native_key_code_(native_key_code),
        key_up_(key_up),
        is_char_(is_char),
        character_(character),
        unmodified_character_(unmodified_character),
        modifiers_(modifiers) {}

  void Execute() override {
    if (!browser_) {
      return;
    }
    CefKeyEvent ev;
    ev.modifiers = modifiers_;
    ev.windows_key_code = windows_key_code_;
    ev.native_key_code = native_key_code_;
    ev.is_system_key = 0;
    // macOS OSR requires character/unmodified_character (cefclient + ceftests).
    ev.character = static_cast<char16_t>(character_);
    ev.unmodified_character = static_cast<char16_t>(unmodified_character_);
    if (is_char_) {
      ev.type = KEYEVENT_CHAR;
    } else if (key_up_) {
      ev.type = KEYEVENT_KEYUP;
    } else {
#if defined(OS_MAC)
      // macOS OSR text_input_client uses KEYDOWN (not RAWKEYDOWN) before CHAR.
      ev.type = KEYEVENT_KEYDOWN;
#else
      ev.type = KEYEVENT_RAWKEYDOWN;
#endif
    }
    browser_->GetHost()->SendKeyEvent(ev);
  }

 private:
  CefRefPtr<CefBrowser> browser_;
  int windows_key_code_;
  int native_key_code_;
  bool key_up_;
  bool is_char_;
  uint32_t character_;
  uint32_t unmodified_character_;
  uint32_t modifiers_;
  IMPLEMENT_REFCOUNTING(KeyEventTask);
};

class FocusTask : public CefTask {
 public:
  FocusTask(CefRefPtr<CefBrowser> browser, bool focus)
      : browser_(std::move(browser)), focus_(focus) {}

  void Execute() override {
    if (!browser_) {
      return;
    }
    browser_->GetHost()->SetFocus(focus_);
  }

 private:
  CefRefPtr<CefBrowser> browser_;
  bool focus_;
  IMPLEMENT_REFCOUNTING(FocusTask);
};

class ImeCommitTextTask : public CefTask {
 public:
  ImeCommitTextTask(CefRefPtr<CefBrowser> browser, std::string utf8)
      : browser_(std::move(browser)), utf8_(std::move(utf8)) {}

  void Execute() override {
    if (!browser_ || utf8_.empty()) {
      return;
    }
    browser_->GetHost()->ImeCommitText(utf8_, CefRange::InvalidRange(), 0);
  }

 private:
  CefRefPtr<CefBrowser> browser_;
  std::string utf8_;
  IMPLEMENT_REFCOUNTING(ImeCommitTextTask);
};

class VtTryTask : public CefTask {
 public:
  explicit VtTryTask(CefRefPtr<CefOsClient> client)
      : client_(std::move(client)) {}

  void Execute() override {
    if (client_) {
      client_->TryConsumeVtCredit();
    }
  }

 private:
  CefRefPtr<CefOsClient> client_;
  IMPLEMENT_REFCOUNTING(VtTryTask);
};

}  // namespace

CefOsClient::CefOsClient(int width, int height,
                         std::shared_ptr<CefOsFrame> frame)
    : view_width_(std::max(width, 1)),
      view_height_(std::max(height, 1)),
      frame_(std::move(frame)) {}

void CefOsClient::SetHtmlBrowserListener(HtmlBrowserListener* listener) {
  std::lock_guard<std::mutex> lock(mutex_);
  listener_ = listener;
}

void CefOsClient::SetViewSize(int width, int height) {
  view_width_ = std::max(width, 1);
  view_height_ = std::max(height, 1);
}

bool CefOsClient::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefProcessId source_process,
    CefRefPtr<CefProcessMessage> message) {
  if (message_router_ &&
      message_router_->OnProcessMessageReceived(browser, frame, source_process,
                                                message)) {
    return true;
  }
  return false;
}

bool CefOsClient::OnBeforeBrowse(CefRefPtr<CefBrowser> browser,
                                 CefRefPtr<CefFrame> frame,
                                 CefRefPtr<CefRequest> /*request*/,
                                 bool /*user_gesture*/,
                                 bool /*is_redirect*/) {
  if (message_router_) {
    message_router_->OnBeforeBrowse(browser, frame);
  }
  return false;
}

void CefOsClient::OnRenderProcessTerminated(CefRefPtr<CefBrowser> browser,
                                            TerminationStatus /*status*/,
                                            int /*error_code*/,
                                            const CefString& /*error_string*/) {
  if (message_router_) {
    message_router_->OnRenderProcessTerminated(browser);
  }
}

bool CefOsClient::OnQuery(CefRefPtr<CefBrowser> /*browser*/,
                          CefRefPtr<CefFrame> /*frame*/,
                          int64_t /*query_id*/,
                          const CefString& request,
                          bool /*persistent*/,
                          CefRefPtr<Callback> callback) {
  // Convention: request = "mc.html.ready" | "mc.html.error:<msg>" |
  //             "mc.html.seekDone:<t_ms>" |
  //             "mc.html.rects:<json>" (T2 phase 1, see html_element_meta.h —
  //             raw JSON forwarded; parse/merge owned by the listener).
  const std::string req = request.ToString();
  HtmlBrowserListener* listener = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    listener = listener_;
  }

  constexpr const char kReady[] = "mc.html.ready";
  constexpr const char kError[] = "mc.html.error:";
  constexpr const char kSeekDone[] = "mc.html.seekDone:";
  constexpr const char kApplied[] = "mc.html.applied:";
  constexpr const char kRects[] = "mc.html.rects:";

  if (req == kReady) {
    if (listener) {
      listener->OnHtmlReady();
    }
    if (callback) {
      callback->Success("");
    }
    return true;
  }
  if (req.rfind(kError, 0) == 0) {
    const std::string msg = req.substr(sizeof(kError) - 1);
    if (listener) {
      listener->OnHtmlError(msg);
    }
    if (callback) {
      callback->Success("");
    }
    return true;
  }
  if (req.rfind(kSeekDone, 0) == 0) {
    const std::string t_str = req.substr(sizeof(kSeekDone) - 1);
    char* end = nullptr;
    const long long parsed = std::strtoll(t_str.c_str(), &end, 10);
    const int64_t t_ms =
        (end && end != t_str.c_str()) ? static_cast<int64_t>(parsed) : 0;
    if (listener) {
      listener->OnHtmlSeekDone(t_ms);
    }
    if (callback) {
      callback->Success("");
    }
    return true;
  }
  if (req.rfind(kApplied, 0) == 0) {
    const int64_t t =
        std::strtoll(req.substr(sizeof(kApplied) - 1).c_str(), nullptr, 10);
    if (listener_) {
      listener_->OnTimelineApplied(t);
    }
    return true;
  }
  if (req.rfind(kRects, 0) == 0) {
    const std::string payload = req.substr(sizeof(kRects) - 1);
    if (listener) {
      listener->OnHtmlRects(payload);
    }
    if (callback) {
      callback->Success("");
    }
    return true;
  }
  return false;
}

CefRefPtr<CefBrowser> CefOsClient::browser() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return browser_;
}

void CefOsClient::RequestClose() {
  CefRefPtr<CefBrowser> to_close;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_close_ = true;
    to_close = browser_;
  }
  if (to_close) {
    CefPostTask(TID_UI, new CloseBrowserTask(std::move(to_close)));
  }
}

void CefOsClient::QueueLoadUrl(std::string url) {
  CefRefPtr<CefBrowser> browser;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_close_) {
      return;
    }
    if (!browser_) {
      pending_url_ = std::move(url);
      has_pending_url_ = true;
      return;
    }
    browser = browser_;
  }
  CefPostTask(TID_UI, new NavigateTask(std::move(browser), std::move(url)));
}

void CefOsClient::QueueExecuteJavaScript(std::string code) {
  CefRefPtr<CefBrowser> browser;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_close_) {
      return;
    }
    if (!browser_) {
      pending_js_ = std::move(code);
      has_pending_js_ = true;
      return;
    }
    browser = browser_;
  }
  CefPostTask(TID_UI, new ExecuteJsTask(std::move(browser), std::move(code)));
}

void CefOsClient::QueueSendMouseClick(int x, int y, int button, bool mouse_up,
                                      uint32_t modifiers) {
  CefRefPtr<CefBrowser> browser;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_close_ || !browser_) {
      return;
    }
    browser = browser_;
  }
  CefPostTask(TID_UI, new MouseClickTask(std::move(browser), x, y, button,
                                         mouse_up, modifiers));
}

void CefOsClient::QueueSendMouseMove(int x, int y, bool mouse_leave,
                                     uint32_t modifiers) {
  CefRefPtr<CefBrowser> browser;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_close_ || !browser_) {
      return;
    }
    browser = browser_;
  }
  CefPostTask(TID_UI, new MouseMoveTask(std::move(browser), x, y, mouse_leave,
                                        modifiers));
}

void CefOsClient::QueueSendMouseWheel(int x, int y, int delta_x, int delta_y,
                                      uint32_t modifiers) {
  CefRefPtr<CefBrowser> browser;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_close_ || !browser_) {
      return;
    }
    browser = browser_;
  }
  CefPostTask(TID_UI, new MouseWheelTask(std::move(browser), x, y, delta_x,
                                         delta_y, modifiers));
}

void CefOsClient::QueueSendKeyEvent(int windows_key_code, int native_key_code,
                                    bool key_up, bool is_char,
                                    uint32_t character,
                                    uint32_t unmodified_character,
                                    uint32_t modifiers) {
  CefRefPtr<CefBrowser> browser;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_close_ || !browser_) {
      return;
    }
    browser = browser_;
  }
  CefPostTask(TID_UI,
              new KeyEventTask(std::move(browser), windows_key_code,
                               native_key_code, key_up, is_char, character,
                               unmodified_character, modifiers));
}

void CefOsClient::QueueSetFocus(bool focus) {
  CefRefPtr<CefBrowser> browser;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_close_ || !browser_) {
      return;
    }
    browser = browser_;
  }
  CefPostTask(TID_UI, new FocusTask(std::move(browser), focus));
}

void CefOsClient::QueueImeCommitText(std::string utf8_text) {
  CefRefPtr<CefBrowser> browser;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_close_ || !browser_ || utf8_text.empty()) {
      return;
    }
    browser = browser_;
  }
  CefPostTask(TID_UI,
              new ImeCommitTextTask(std::move(browser), std::move(utf8_text)));
}

void CefOsClient::GetViewRect(CefRefPtr<CefBrowser> /*browser*/,
                              CefRect& rect) {
  // Called on CEF UI thread. Provide non-empty DIP rect for OSR.
  // device_scale_factor is forced to 1.0 in GetScreenInfo so DIP == pixels.
  rect = CefRect(0, 0, view_width_, view_height_);
}

bool CefOsClient::GetScreenInfo(CefRefPtr<CefBrowser> /*browser*/,
                                CefScreenInfo& screen_info) {
  // Keep 1:1 DIP↔pixel so OnPaint size matches html_width/height.
  screen_info.device_scale_factor = 1.0f;
  screen_info.depth = 32;
  screen_info.depth_per_component = 8;
  screen_info.is_monochrome = false;
  screen_info.rect = CefRect(0, 0, view_width_, view_height_);
  screen_info.available_rect = screen_info.rect;
  return true;
}

void CefOsClient::OnPaint(CefRefPtr<CefBrowser> /*browser*/,
                          PaintElementType type,
                          const RectList& /*dirtyRects*/,
                          const void* buffer,
                          int width,
                          int height) {
  // CEF UI thread: CPU copy only. No GL.
  if (type != PET_VIEW || !buffer || !frame_ || width <= 0 || height <= 0) {
    return;
  }

  static bool logged_first_paint = false;
  if (!logged_first_paint) {
    logged_first_paint = true;
    LOG(INFO) << "[cef] OnPaint first PET_VIEW " << width << "x" << height
              << "\n";
  }

  const size_t nbytes =
      static_cast<size_t>(width) * static_cast<size_t>(height) * 4u;
  std::lock_guard<std::mutex> lock(frame_->mutex);
  frame_->width = width;
  frame_->height = height;
  frame_->bgra.resize(nbytes);
  // Full-frame copy (dirty-rect path deferred — YAGNI until perf needs it).
  std::memcpy(frame_->bgra.data(), buffer, nbytes);
  frame_->dirty = true;

  if (vt_enabled_) {
    OnVtPaintReceived();
  }
}

void CefOsClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  // CEF UI thread. Apply any close/nav/JS queued before the browser existed.
  bool should_close = false;
  bool has_url = false;
  bool has_js = false;
  std::string url;
  std::string code;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    browser_ = browser;
    should_close = pending_close_;
    if (!should_close) {
      if (has_pending_url_) {
        url = std::move(pending_url_);
        has_pending_url_ = false;
        has_url = true;
      }
      if (has_pending_js_) {
        code = std::move(pending_js_);
        has_pending_js_ = false;
        has_js = true;
      }
    } else {
      has_pending_url_ = false;
      has_pending_js_ = false;
      pending_url_.clear();
      pending_js_.clear();
    }
  }

  // One browser-side router per OSR client; Handler = this.
  if (!message_router_) {
    CefMessageRouterConfig config;
    message_router_ = CefMessageRouterBrowserSide::Create(config);
    if (message_router_) {
      message_router_->AddHandler(this, /*first=*/false);
    } else {
      LOG(ERROR) << "[cef] CefMessageRouterBrowserSide::Create failed\n";
    }
  }

  CefTrackOsBrowserCreated();

  if (should_close) {
    browser->GetHost()->CloseBrowser(/*force_close=*/true);
    return;
  }

  // OSR: announce view size so the renderer produces the first PET_VIEW paint.
  browser->GetHost()->WasResized();
  browser->GetHost()->Invalidate(PET_VIEW);
  LOG(INFO) << "[cef] OnAfterCreated id=" << browser->GetIdentifier()
            << " view=" << view_width_ << "x" << view_height_ << "\n";

  if (has_url) {
    CefRefPtr<CefFrame> frame = browser->GetMainFrame();
    if (frame) {
      frame->LoadURL(url);
    }
  }
  if (has_js) {
    CefRefPtr<CefFrame> frame = browser->GetMainFrame();
    if (frame) {
      frame->ExecuteJavaScript(code, frame->GetURL(), 0);
    }
  }
}

void CefOsClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  vt_devtools_reg_ = nullptr;
  vt_enabled_ = false;
  vt_credit_.store(0, std::memory_order_relaxed);
  vt_state_ = VtState::Idle;
  if (message_router_) {
    message_router_->OnBeforeClose(browser);
    message_router_->RemoveHandler(this);
    message_router_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    browser_ = nullptr;
  }
  CefTrackOsBrowserClosed();
}

void CefOsClient::OnLoadEnd(CefRefPtr<CefBrowser> browser,
                            CefRefPtr<CefFrame> frame,
                            int httpStatusCode) {
  if (!browser || !frame || !frame->IsMain()) {
    return;
  }
  LOG(INFO) << "[cef] OnLoadEnd status=" << httpStatusCode
            << " url=" << frame->GetURL().ToString() << "\n";
  // 采集/时序脚本重注入：browser create 时的首次注入发生在 load 之前，
  // 页面文档替换会丢掉 window 上的函数（实测前两帧 ApplyTimeline 落空）。
  // 脚本自带幂等标志，重复注入无害。
  frame->ExecuteJavaScript(MakeRectsScript(), frame->GetURL(), 0);
  if (vt_enabled_) {
    vt_load_finished_ = true;
    // Kick first VT step if Graphics already posted credit.
    TryConsumeVtCredit();
  } else {
    browser->GetHost()->Invalidate(PET_VIEW);
  }
}

void CefOsClient::OnLoadError(CefRefPtr<CefBrowser> browser,
                              CefRefPtr<CefFrame> frame,
                              ErrorCode errorCode,
                              const CefString& errorText,
                              const CefString& failedUrl) {
  if (!frame || !frame->IsMain()) {
    return;
  }
  LOG(ERROR) << "[cef] OnLoadError code=" << errorCode
            << " text=" << errorText.ToString()
            << " url=" << failedUrl.ToString() << "\n";
  (void)browser;
}

void CefOsClient::EnableVirtualTime(int fps) {
  // Called on main/CEF UI after CreateBrowserSync (browser_ already set).
  vt_enabled_ = true;
  vt_fps_ = std::max(1, std::min(fps > 0 ? fps : 30, 60));
  vt_state_ = VtState::Idle;
  chrome_vt_ms_ = 0;
  CefRefPtr<CefBrowser> b = browser();
  if (!b) {
    LOG(ERROR) << "[cef] EnableVirtualTime: no browser yet\n";
    return;
  }
  vt_devtools_reg_ = b->GetHost()->AddDevToolsMessageObserver(this);
  if (!vt_devtools_reg_) {
    LOG(ERROR) << "[cef] AddDevToolsMessageObserver failed (VT disabled)\n";
    vt_enabled_ = false;
    return;
  }
  LOG(ERROR) << "[cef] VirtualTime enabled fps=" << vt_fps_ << "\n";
}

void CefOsClient::RequestVtStep() {
  if (!vt_enabled_) {
    return;
  }
  vt_credit_.fetch_add(1, std::memory_order_acq_rel);
  const bool posted = CefPostTask(TID_UI, new VtTryTask(this));
}

void CefOsClient::InvalidateView() {
  CefRefPtr<CefBrowser> b = browser();
  if (b) {
    b->GetHost()->Invalidate(PET_VIEW);
  }
}

void CefOsClient::TryConsumeVtCredit() {
  if (!vt_enabled_) {
    return;
  }
  // C2: if stuck waiting for paint, release so later credits can re-arm.
  if (vt_state_ == VtState::WaitPaint) {
    vt_state_ = VtState::Pause;
    int prev = vt_credit_.load(std::memory_order_acquire);
    while (prev > 0 &&
           !vt_credit_.compare_exchange_weak(prev, prev - 1,
                                             std::memory_order_acq_rel)) {
    }
  }
  if (vt_state_ == VtState::Arming || vt_state_ == VtState::WaitBudget) {
    return;
  }
  if (vt_credit_.load(std::memory_order_acquire) <= 0) {
    vt_state_ = VtState::Pause;
    return;
  }
  // Do NOT gate on OnLoadEnd: with external_begin_frame, the renderer may not
  // finish load until VirtualTime advances (mix re-arms VT while loading).
  ArmVirtualTimePolicy();
}

void CefOsClient::ArmVirtualTimePolicy() {
  CefRefPtr<CefBrowser> b = browser();
  if (!b) {
    return;
  }
  // Budget ~40ms matches mix MixCefBrowserV2; chrome_vt_ms_ steps by 1000/fps.
  constexpr int kBudgetMs = 40;
  const std::string params_json =
      std::string("{\"policy\":\"pauseIfNetworkFetchesPending\",\"budget\":") +
      std::to_string(kBudgetMs) + "}";
  CefRefPtr<CefDictionaryValue> dict;
  CefRefPtr<CefValue> value =
      CefParseJSON(params_json.data(), params_json.size(), JSON_PARSER_RFC);
  if (value && value->GetType() == VTYPE_DICTIONARY) {
    dict = value->GetDictionary();
  }
  if (!dict) {
    if (!vt_warned_fail_) {
      vt_warned_fail_ = true;
      LOG(ERROR) << "[cef] CefParseJSON for setVirtualTimePolicy failed\n";
    }
    vt_state_ = VtState::Pause;
    return;
  }
  vt_state_ = VtState::Arming;
  static int s_vt_msg_id = 1;
  const int mid = b->GetHost()->ExecuteDevToolsMethod(
      s_vt_msg_id++, "Emulation.setVirtualTimePolicy", dict);
  if (mid == 0) {
    if (!vt_warned_fail_) {
      vt_warned_fail_ = true;
      LOG(ERROR) << "[cef] ExecuteDevToolsMethod setVirtualTimePolicy failed\n";
    }
    vt_state_ = VtState::Pause;
    return;
  }
  vt_state_ = VtState::WaitBudget;
}

bool CefOsClient::OnDevToolsMessage(CefRefPtr<CefBrowser> /*browser*/,
                                    const void* message,
                                    size_t message_size) {
  if (!vt_enabled_ || !message || message_size == 0) {
    return false;
  }
  const std::string msg(static_cast<const char*>(message), message_size);
  if (msg.find("Emulation.virtualTimeBudgetExpired") != std::string::npos) {
    OnVirtualTimeBudgetExpired();
    return true;
  }
  return false;
}

void CefOsClient::OnVirtualTimeBudgetExpired() {
  if (!vt_enabled_) {
    return;
  }
  chrome_vt_ms_ += 1000 / std::max(vt_fps_, 1);
  CefRefPtr<CefBrowser> b = browser();
  if (!b) {
    vt_state_ = VtState::Pause;
    return;
  }
  if (!vt_load_finished_) {
    // mix: load not finish → re-arm VT policy (no BeginFrame yet).
    ArmVirtualTimePolicy();
    return;
  }
  vt_state_ = VtState::WaitPaint;
  // 静态页面在 VT 步进间没有新损伤：external begin frame 只合成已损伤
  // 内容，无损伤则 BeginFrame 不产出 OnPaint（实测第二次步进起卡死）。
  // Invalidate 强制全视图重绘，保证每步确定出一帧——bake 的确定性靠
  // VT 时钟而不是靠损伤来源。
  b->GetHost()->Invalidate(PET_VIEW);
  b->GetHost()->SendExternalBeginFrame();
}

void CefOsClient::OnVtPaintReceived() {
  // Called from OnPaint on CEF UI after writing CefOsFrame.
  if (vt_state_ != VtState::WaitPaint) {
    return;
  }
  // Consume one credit for this VT step (floor at 0).
  int prev = vt_credit_.load(std::memory_order_acquire);
  while (prev > 0 &&
         !vt_credit_.compare_exchange_weak(prev, prev - 1,
                                           std::memory_order_acq_rel)) {
  }
  vt_state_ = VtState::Pause;
  if (vt_credit_.load(std::memory_order_acquire) > 0) {
    TryConsumeVtCredit();
  }
}

}  // namespace cef
}  // namespace xmedia

#endif  // XMEDIA_HAS_CEF

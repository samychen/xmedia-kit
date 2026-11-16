// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_CEF_CEF_OSR_TASKS_H_
#define XMEDIA_MEDIA_CEF_CEF_OSR_TASKS_H_

// CefOsClient 的 CEF 线程投递封装。
//
// CEF 要求 browser 操作在 TID_UI 上执行，而调用方（渲染线程、宿主）不在
// 那条线程上，所以每个动作都是一个 CefTask：把参数拷进对象，由
// CefPostTask 投递，Execute() 在目标线程上跑。
//
// 拆出本文件的理由与 html_bake_video_layer 相同：这些类只依赖 CEF 的类型
// （CefBrowser/CefFrame/CefTask），与 CefOsClient 的 OSR 状态（view 尺寸、
// 帧缓冲、JS 队列）不共享成员——唯一的例外是 VtTryTask，它调
// CefOsClient::TryConsumeVtCredit()，而那是 public 方法。
//
// 类定义整体搬自 cef_osr_client.cc（2026-11-15），逻辑未改动。

#include <string>
#include <utility>

#include "media/cef/cef_osr_client.h"

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF

#include "include/cef_browser.h"
#include "include/cef_frame.h"
#include "include/cef_task.h"

namespace xmedia {
namespace cef {
namespace internal {

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

}  // namespace internal
}  // namespace cef
}  // namespace xmedia

#endif  // defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF

#endif  // XMEDIA_MEDIA_CEF_CEF_OSR_TASKS_H_

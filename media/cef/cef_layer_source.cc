// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CefLayerSource 实现。线程模型见头文件：Start/Stop 宿主线程；pump 线程
// 持有 CefInitialize（runtime 的同线程契约）并以看门狗频率泵；OnPaint 的
// BGRA 帧在 pump 线程转成 kARGB VideoFrame 走 GlLayerSource::Publish——
// 内存序 B,G,R,A 与 kARGB（AV_PIX_FMT_0RGB32）一致，无字节交换。
// 元数据采集：OnLoadEnd 注入 cef::MakeRectsScript()（一次性），上报经
// MessageRouter 回到 HtmlBrowserListener::OnHtmlRects → ParseHtmlRectsPayload
// → 表合并。

#include "media/cef/cef_layer_source.h"

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>

#include "base/logging.h"
#include <cstdio>

#include "media/cef/cef_osr_client.h"
#include "media/cef/cef_runtime.h"

namespace xmedia::media {

struct CefLayerSource::Impl : public cef::HtmlBrowserListener {
  Config config;
  std::unique_ptr<GlLayerSource> source;
  std::shared_ptr<cef::CefOsFrame> frame =
      std::make_shared<cef::CefOsFrame>();
  cef::CefRuntime& runtime = cef::CefRuntime::Instance();
  cef::BrowserHandle browser;

  // HyperFrames 元数据：采集脚本上报 → 解析合并。pump 线程写；宿主在
  // Stop 之后读（无并发）或运行中自持锁。
  HtmlElementMetaTable elements;
  bool any_report = false;
  bool rects_injected = false;

  std::thread pump;
  std::atomic<bool> running{false};
  int64_t start_wall_ms = 0;
  int64_t published_frames = 0;
  // 最近一次发布的 OSR 帧（元素拆纹理的 GlElementComposer 从这里取整层
  // 图去各元素层上传）。pump 写、宿主读，互斥保护 scoped_refptr 本体。
  std::mutex last_frame_mutex;
  base::scoped_refptr<VideoFrame> last_published;

  int64_t applied_t_ms = -1;

  // 时序应用回执：渲染进程执行到的时间（单调）。
  std::mutex applied_mutex;
  std::condition_variable applied_cv;

  // 页面数据注入：SetHtmlData 暂存，首次出帧后注入一次。
  std::mutex data_mutex;
  std::string pending_data_json;
  bool data_injected = false;

  // seek 时钟：seekDone 到达时间（单调）+ 页面无 window.seek 标记。
  std::mutex seek_mutex;
  std::condition_variable seek_cv;
  int64_t seek_done_t_ms = -1;
  bool seek_unsupported_ = false;

  // HtmlBrowserListener（CEF UI 线程）。
  void OnHtmlReady() override {}
  void OnHtmlError(const std::string& message) override {
    LOG(ERROR) << "cef layer: html error: " << message;
  }
  void OnHtmlSeekDone(int64_t) override {}
  void OnTimelineApplied(int64_t t_ms) override {
    std::lock_guard<std::mutex> scoped(applied_mutex);
    applied_t_ms = std::max(applied_t_ms, t_ms);
    applied_cv.notify_all();
  }
  void OnHtmlRects(const std::string& payload_json) override {
    HtmlRectsSnapshot snap;
    if (!ParseHtmlRectsPayload(payload_json, &snap)) {
      LOG(ERROR) << "cef layer: rects payload parse failed";
      return;
    }
    std::lock_guard<std::mutex> scoped(elements_mutex);
    elements.Merge(snap);
    any_report = true;
  }

  // Host-side snapshot: copies under the mutex instead of exposing the live
  // table (elements() stays for the post-Stop read).
  HtmlElementMetaTable SnapshotElements() const {
    std::lock_guard<std::mutex> scoped(elements_mutex);
    return elements;
  }

  mutable std::mutex elements_mutex;

  void PublishFrameIfDirty() {
    int w = 0;
    int h = 0;
    {
      std::lock_guard<std::mutex> scoped(frame->mutex);
      if (!frame->dirty || frame->width <= 0 || frame->height <= 0) {
        return;
      }
      w = frame->width;
      h = frame->height;
    }
    auto out = VideoFrame::CreateBlackFrame(
        VideoFormat::kARGB, Size{w, h}, Size{w, h}, Rational{1, 1},
        base::TimeDelta::FromMilliseconds(
            static_cast<int64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count()) -
            start_wall_ms),
        base::TimeDelta::FromMilliseconds(1000 / std::max(1, config.fps)),
        /*serial=*/0);
    const size_t bytes = static_cast<size_t>(w) * h * 4;
    auto span = out->mutable_data(VideoFrame::Plane::kYPlane);
    if (span.size() < bytes) {
      LOG(ERROR) << "cef layer: frame plane smaller than OnPaint payload";
      std::lock_guard<std::mutex> scoped(frame->mutex);
      frame->dirty = false;
      return;
    }
    {
      std::lock_guard<std::mutex> scoped(frame->mutex);
      memcpy(span.data(), frame->bgra.data(), bytes);
      frame->dirty = false;
    }
    {
      std::lock_guard<std::mutex> scoped(last_frame_mutex);
      last_published = out;  // Publish 之后 out 已被搬空，先存引用
    }
    source->Publish(std::move(out));
    ++published_frames;
    // 数据注入时机（MC FlushPendingDataIfReady 对齐）：首次出帧后一次。
    std::string data;
    {
      std::lock_guard<std::mutex> scoped(data_mutex);
      if (!data_injected && published_frames >= 1 &&
          !pending_data_json.empty()) {
        data = pending_data_json;
        data_injected = true;
      }
    }
    if (!data.empty() && browser) {
      runtime.ExecuteJavaScript(browser, cef::MakeUpdateScript(data));
    }
  }

  void PumpLoop() {
    while (running.load(std::memory_order_acquire)) {
      runtime.DoMessageLoopWork();
      PublishFrameIfDirty();
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
};

CefLayerSource::CefLayerSource() = default;

CefLayerSource::~CefLayerSource() {
  Stop();
}

bool CefLayerSource::Start(const Config& config) {
  if (impl_ && impl_->running.load()) {
    return false;
  }
  auto impl = std::make_unique<Impl>();
  impl->config = config;
  impl->source = std::make_unique<GlLayerSource>(config.name, config.z);
  impl->start_wall_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count();

  // GUI 宿主（GLFW/NSApplication 占据主线程 run loop）必须把 CEF 的
  // Init/DoMessageLoopWork 放在主线程：CEF macOS 启动期要向主线程
  // dispatch，pump 线程上的 Init 会与主线程 run loop 互等（实测死锁在
  // CefInitialize 内）。pump_on_caller_thread=true 时 Start 内联全部
  // 初始化，宿主随后每帧调用 Pump()。
  auto bootstrap = [impl_raw = impl.get()]() -> bool {
    std::string framework_dir = impl_raw->config.framework_dir;
    std::string subprocess = impl_raw->config.subprocess_path;
    if (framework_dir.empty() || subprocess.empty()) {
      cef::CefDeployPaths paths;
      if (cef::CefBundlePaths(&paths)) {
        if (framework_dir.empty()) {
          framework_dir = paths.framework_dir;
        }
        if (subprocess.empty()) {
          subprocess = paths.subprocess_path;
        }
      }
    }
    if (!impl_raw->runtime.Init(framework_dir, subprocess)) {
      LOG(ERROR) << "cef layer: CEF runtime init failed";
      impl_raw->running.store(false);
      return false;
    }
    impl_raw->browser = impl_raw->runtime.CreateOsBrowser(
        impl_raw->config.url, impl_raw->config.width,
        impl_raw->config.height, impl_raw->config.transparent,
        impl_raw->config.fps, impl_raw->frame, impl_raw,
        /*external_begin_frame=*/impl_raw->config.bake_virtual_time);
    if (!impl_raw->browser) {
      LOG(ERROR) << "cef layer: OSR browser create failed";
      impl_raw->running.store(false);
      return false;
    }
    // 采集脚本注入（OnLoadEnd 之外的一次性保底；collector 自带幂等标志）。
    impl_raw->runtime.ExecuteJavaScript(impl_raw->browser,
                                        cef::MakeRectsScript());
    return true;
  };

  if (impl->config.pump_on_caller_thread) {
    // 先移交所有权再引导：bootstrap 捕获的是裸指针（对象此后由 impl_
    // 持有），Stop/last_frame/SnapshotElements 都走成员。
    impl_ = std::move(impl);
    impl_->running.store(true);
    bootstrap();
    return impl_->running.load();
  }

  // Runtime 契约（Init/pump 同线程）由 pump 线程自己满足：Start 只负责
  // 起线程，Init/CreateOsBrowser/Pump 全部发生在该线程上。
  impl->running.store(true);
  impl->pump = std::thread([impl_raw = impl.get(), bootstrap]() {
    if (!bootstrap()) {
      return;
    }
    impl_raw->PumpLoop();
  });
  impl_ = std::move(impl);
  // 给 pump 一拍时间让 is_running() 反映 init 结果。
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return impl_->running.load();
}

void CefLayerSource::Stop() {
  if (!impl_) {
    return;
  }
  // 先停 pump（避免并发 pump 与 drain），再关浏览器并 drain。
  impl_->running.store(false);
  if (impl_->pump.joinable()) {
    impl_->pump.join();
  }
  if (impl_->browser) {
    impl_->runtime.CloseBrowser(impl_->browser);
    impl_->browser.reset();
  }
  impl_->runtime.Shutdown();
  // impl_ 保留到析构：宿主 Stop 后仍可读 elements()（无并发）。
}

GlLayerSource* CefLayerSource::source() {
  return impl_ ? impl_->source.get() : nullptr;
}

void CefLayerSource::RequestVtStep() {
  if (impl_ && impl_->browser) {
    impl_->runtime.RequestVtStep(impl_->browser);
  }
}

bool CefLayerSource::AwaitTimelineApplied(int64_t t_ms,
                                        int timeout_ms) {
  if (impl_ == nullptr) {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  // 自旋而非 CV 等待：caller-thread 模式下回执需要本线程 Pump 才能被
  // 处理，wait_until 阻塞会把处理者卡死在等待里（实测每次 await 都
  // 耗满超时后才能在下一次 Pump 中看到回执）。
  while (true) {
    if (!impl_->running.load()) {
      return false;
    }
    if (impl_->config.pump_on_caller_thread) {
      impl_->runtime.DoMessageLoopWork();
    }
    {
      std::lock_guard<std::mutex> lock(impl_->applied_mutex);
      if (impl_->applied_t_ms >= t_ms) {
        return true;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

bool CefLayerSource::SetHtmlData(std::string json_utf8) {
  if (!cef::ValidateHtmlDataJson(json_utf8)) {
    return false;
  }
  if (impl_ == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> scoped(impl_->data_mutex);
  impl_->pending_data_json = std::move(json_utf8);
  impl_->data_injected = false;
  return true;
}

bool CefLayerSource::seek_unsupported() const {
  return impl_ && impl_->seek_unsupported_;
}

void CefLayerSource::SeekDocument(int64_t t_ms) {
  if (impl_ && impl_->browser) {
    impl_->runtime.ExecuteJavaScript(impl_->browser,
                                     cef::MakeSeekScript(t_ms));
  }
}

bool CefLayerSource::AwaitSeekDone(int64_t t_ms, int timeout_ms) {
  if (impl_ == nullptr) {
    return false;
  }
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
  while (true) {
    if (!impl_->running.load()) {
      return false;
    }
    if (impl_->config.pump_on_caller_thread) {
      impl_->runtime.DoMessageLoopWork();
    }
    {
      std::lock_guard<std::mutex> scoped(impl_->seek_mutex);
      if (impl_->seek_unsupported_) {
        return false;
      }
      if (impl_->seek_done_t_ms >= t_ms) {
        return true;
      }
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

void CefLayerSource::ApplyTimeline(int64_t t_ms) {
  if (impl_ && impl_->browser) {
    impl_->runtime.ExecuteJavaScript(
        impl_->browser,
        "window.__mcApplyTimeline ? window.__mcApplyTimeline(" +
            std::to_string(t_ms) + ") : (window.cefQuery && "
            "window.cefQuery({request:'mc.html.error:no_apply',"
            "persistent:false,onSuccess:function(){},onFailure:function(){}}));");
  }
}

void CefLayerSource::Invalidate() {
  if (impl_ && impl_->browser) {
    impl_->runtime.Invalidate(impl_->browser);
  }
}

void CefLayerSource::Pump() {
  if (impl_ == nullptr || !impl_->running.load()) {
    return;
  }
  impl_->runtime.DoMessageLoopWork();
  impl_->PublishFrameIfDirty();
}

HtmlElementMetaTable CefLayerSource::SnapshotElements() const {
  return impl_ ? impl_->SnapshotElements() : HtmlElementMetaTable();
}

base::scoped_refptr<VideoFrame> CefLayerSource::last_frame() const {
  if (!impl_) {
    return nullptr;
  }
  std::lock_guard<std::mutex> scoped(impl_->last_frame_mutex);
  return impl_->last_published;
}

const HtmlElementMetaTable& CefLayerSource::elements() const {
  static const HtmlElementMetaTable empty;
  return impl_ ? impl_->elements : empty;
}

bool CefLayerSource::is_running() const {
  return impl_ && impl_->running.load();
}

}  // namespace xmedia::media

#endif  // XMEDIA_HAS_CEF

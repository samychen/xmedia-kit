// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// CefLayerSource 实现。线程模型见头文件：Start/Stop 宿主线程；pump 线程
// 持有 CefInitialize（runtime 的同线程契约）并以看门狗频率泵；OnPaint 的
// BGRA 帧在 pump 线程转成 kARGB VideoFrame 走 GlLayerSource::Publish——
// 内存序 B,G,R,A 与 kARGB（AV_PIX_FMT_0RGB32）一致，无字节交换。
// 元数据采集：OnLoadEnd 注入 cefc::MakeRectsScript()（一次性），上报经
// MessageRouter 回到 HtmlBrowserListener::OnHtmlRects → ParseHtmlRectsPayload
// → 表合并。

#include "media/cef/cef_layer_source.h"

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF

#include <chrono>
#include <cstring>
#include <thread>
#include <utility>

#include "base/logging.h"
#include "media/cef/cefc_rects_script.h"
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

  // HtmlBrowserListener（CEF UI 线程）。
  void OnHtmlReady() override {}
  void OnHtmlError(const std::string& message) override {
    LOG(ERROR) << "cef layer: html error: " << message;
  }
  void OnHtmlSeekDone(int64_t) override {}
  void OnHtmlRects(const std::string& payload_json) override {
    HtmlRectsSnapshot snap;
    if (!ParseHtmlRectsPayload(payload_json, &snap)) {
      LOG(ERROR) << "cef layer: rects payload parse failed";
      return;
    }
    elements.Merge(snap);
    any_report = true;
  }

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
    source->Publish(std::move(out));
    ++published_frames;
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

  // Runtime 契约（Init/pump 同线程）由 pump 线程自己满足：Start 只负责
  // 起线程，Init/CreateOsBrowser/Pump 全部发生在该线程上。
  impl->running.store(true);
  impl->pump = std::thread([impl_raw = impl.get()]() {
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
      return;
    }
    impl_raw->browser = impl_raw->runtime.CreateOsBrowser(
        impl_raw->config.url, impl_raw->config.width,
        impl_raw->config.height, impl_raw->config.transparent,
        impl_raw->config.fps, impl_raw->frame, impl_raw,
        /*external_begin_frame=*/false);
    if (!impl_raw->browser) {
      LOG(ERROR) << "cef layer: OSR browser create failed";
      impl_raw->running.store(false);
      return;
    }
    // 采集脚本注入（OnLoadEnd 之外的一次性保底；collector 自带幂等标志）。
    impl_raw->runtime.ExecuteJavaScript(impl_raw->browser,
                                        cefc::MakeRectsScript());
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

const HtmlElementMetaTable& CefLayerSource::elements() const {
  static const HtmlElementMetaTable empty;
  return impl_ ? impl_->elements : empty;
}

bool CefLayerSource::is_running() const {
  return impl_ && impl_->running.load();
}

}  // namespace xmedia::media

#endif  // XMEDIA_HAS_CEF

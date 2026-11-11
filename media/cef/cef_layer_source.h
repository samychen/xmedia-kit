// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_CEF_CEF_LAYER_SOURCE_H_
#define XMEDIA_MEDIA_CEF_CEF_LAYER_SOURCE_H_

// CefLayerSource — the HTML composition layer for xmedia-kit's scene:
// a CEF OSR browser whose OnPaint frames flow through the standard
// GlLayerSource mailbox, plus the HyperFrames element-metadata collection
// (media/base/html_element_meta) wired to the mc.html.rects protocol.
//
// PORTRAIT OF THE ORIGINAL. Ported from the composition SDK's
// HtmlLayerProducer + cef_runtime (framework/render/cef), adapted to this
// tree's mailbox model: the SDK uploaded GL textures on a Graphics thread
// (ProcessGpu); here OnPaint frames go straight into GlLayerSource::Publish
// and the scene's own Sync uploads them — the BGRA payload publishes as
// VideoFormat::kARGB (same memory order B,G,R,A), no byte swap.
//
// THREADING. Start/Stop from the host thread. The facade owns a pump thread:
// CefRuntime::Init and DoMessageLoopWork must share one thread (the runtime's
// own contract), so Start runs CefInitialize ON the pump thread and the pump
// calls DoMessageLoopWork at the runtime's watchdog rate until stopped.
// Publish from the pump thread; GlLayerSource takes frames from any thread.
//
// GATE. Everything is compiled only when XMEDIA_HAS_CEF=1 (CMake
// XMEDIA_ENABLE_CEF + a CEF distro at CEF_ROOT); with the gate off the header
// still exists but the facade is absent, and callers probe via
// `xmedia_cef_available()`-style compile-time checks instead.

#include <memory>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "media/base/video_frame.h"

#include "media/base/html_element_meta.h"
#include "media/gl/gl_layer_source.h"
#include "media/media_export.h"

namespace xmedia::media {

#if defined(XMEDIA_HAS_CEF) && XMEDIA_HAS_CEF

class XMEDIA_MEDIA_EXPORT CefLayerSource {
 public:
  struct Config {
    std::string name = "html";
    int z = 10;                    // layer z in the scene (higher = on top)
    int width = 1280;
    int height = 720;
    int fps = 30;                  // windowless_frame_rate cap (1..60)
    bool transparent = false;      // OSR background alpha
    std::string url;               // file:// or https://
    // Explicit CEF deployment override; empty = CefLayerSource resolves the
    // distro layout itself (see cef_runtime CefBundlePaths / Init).
    std::string framework_dir;
    std::string subprocess_path;
    // Bake 时钟（HyperFrames html_bake 对齐）：VirtualTime 驱动——CEF 的
    // external_begin_frame + DevTools setVirtualTimePolicy，每
    // RequestVtStep() 前进一帧间隔，OnPaint 内容确定性推进。bake 输出的
    // 文档时刻 t = 帧序号 * (1000/fps)，与墙钟无关。
    bool bake_virtual_time = false;
    // GUI 宿主模式：CEF 的 Init/pump 留在调用线程（CEF macOS 启动期要向
    // 主线程 dispatch，pump 线程上的 Init 会与主线程 run loop 互等死锁）。
    // true 时宿主每帧调用 Pump() 驱动 CEF；false（默认）= facade 自带
    // pump 线程。
    bool pump_on_caller_thread = false;
  };

  CefLayerSource();
  ~CefLayerSource();
  CefLayerSource(const CefLayerSource&) = delete;
  CefLayerSource& operator=(const CefLayerSource&) = delete;

  // Resolves deploy paths (unless overridden), inits the runtime on the pump
  // thread, creates the OSR browser and loads |config.url|. Returns false
  // with the reason in the log (CEF unavailable, browser create failed).
  bool Start(const Config& config);
  // Closes the browser and shuts the runtime down; the pump thread joins.
  void Stop();

  // The scene consumes this: EnsureLayer/Sync on the GL thread.
  GlLayerSource* source();

  // Element metadata collected from mc.html.rects so far (parse+merge on the
  // pump thread; safe read from the host after Stop, guarded while running).
  const HtmlElementMetaTable& elements() const;

  // Thread-safe copy of the element table while running (the pump merges
  // into it; a bare elements() read would race). Prefer this over elements()
  // whenever the layer has not been stopped.
  HtmlElementMetaTable SnapshotElements() const;

  // The most recent OSR frame published (null before the first OnPaint).
  // The element-composer path uploads this same picture into the per-element
  // scene layers; holding one reference here means the composer does not have
  // to race the mailbox for it.
  base::scoped_refptr<VideoFrame> last_frame() const;

  bool is_running() const;

  // Bake 时钟步进：+1 VT credit，下一帧 OnPaint 以确定性时刻渲染。
  // 仅 bake_virtual_time=true 的层有意义；每个输出帧调用一次。
  void RequestVtStep();

  // caller-thread pump 模式的 CEF 驱动：处理 UI 任务 + 发布脏帧。宿主在
  // 等新帧的循环里反复调用（Pump + 检查 last_frame 变化）。
  void Pump();

  // Bake 时序应用：按 data-start/data-duration 切换页面元素可见性，使
  // 光栅只含 t 时刻生效的元素（每输出帧、RequestVtStep 之前调用）。
  void ApplyTimeline(int64_t t_ms);

  // 强制全视图重绘（bake：时序应用后的确定性出帧）。
  void Invalidate();

  // ---- TickBakeClock（MC 对齐）：统一时钟分发 ----------------------------
  // 每个 compose tick 调一次；按配置分派：
  //   bake_virtual_time=true → 一个 VT credit（忽略 PTS 追赶，VT 自驱）
  //   否则 → seek 时钟：t_ms = media_pts_ms；同 t 去重、seek 在飞去重、
  //   seek_unsupported 短路（回退 live 绘制）——MC TickBakeClock 门控
  //   语义对齐。live（两者皆非）= no-op。
  void TickBakeClock(int64_t media_pts_ms);

  // 等渲染进程执行完 t 的可见性应用（mc.html.applied 回执）。bake 的
  // 确定性前提：步进前确认 DOM 已是 t 的状态。超时返回 false。
  bool AwaitTimelineApplied(int64_t t_ms, int timeout_ms = 2000);

  // ---- Seek bake 时钟（html_bake_clock=seek，MC 对齐） -------------------
  // 页面契约 window.seek(t_ms)：页面自持时间线时由它驱动精确渲染；页面
  // 未实现时（mc.html.error:no_seek 回执）seek_unsupported() 置位，bake
  // 回退 live 墙钟/预应用时序。
  // 页面数据注入（MC SetHtmlData 对齐）：校验通过后暂存，首次出帧后
  // 以 window.update(d) 契约注入一次。校验失败（非 JSON/根非对象数组）
  // 返回 false——原始拼接进 ExecuteJavaScript 等于脚本注入。
  bool SetHtmlData(std::string json_utf8);

  void SeekDocument(int64_t t_ms);
  bool AwaitSeekDone(int64_t t_ms, int timeout_ms = 2000);
  bool seek_unsupported() const;

  // 等渲染进程执行完 t 的可见性应用（mc.html.applied 回执）。bake 的
  // 确定性前提：步进前确认 DOM 已是 t 的状态。超时返回 false。

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

#endif  // XMEDIA_HAS_CEF

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_CEF_CEF_LAYER_SOURCE_H_

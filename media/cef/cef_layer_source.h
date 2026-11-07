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

  // The most recent OSR frame published (null before the first OnPaint).
  // The element-composer path uploads this same picture into the per-element
  // scene layers; holding one reference here means the composer does not have
  // to race the mailbox for it.
  base::scoped_refptr<VideoFrame> last_frame() const;

  bool is_running() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

#endif  // XMEDIA_HAS_CEF

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_CEF_CEF_LAYER_SOURCE_H_

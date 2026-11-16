// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <utility>
#include <vector>

// glad's enumerants BEFORE any host header that could drag in a system GL
// header -- the include-order trap gl_hw_texture_importer.h documents.
#include <glad/gl.h>

#include "media/gl/gl_hw_texture_importer.h"
#if defined(XMEDIA_ENABLE_CUDA_IMPORT)
#include "platform/hwimport/cuda_gl_importer.h"
#endif
#if defined(XMEDIA_ENABLE_EGL_IMPORT)
#include "platform/hwimport/egl_dmabuf_importer.h"
#endif

namespace xmedia::platform::hwimport {
namespace {

// The Linux host may carry MORE THAN ONE import backend: VAAPI frames route
// through EGLImage over derived dmabufs, NVDEC frames route through the
// CUDA-GL interop. The factory hands the sink ONE importer, so the two are
// chained and routed per frame by CanImport -- each backend's CanImport only
// answers yes for the frame shape its producer publishes, so the chain is a
// routing decision, not a trial-and-error loop. The first backend that claims
// the frame owns it; a claim that then fails at ImportPlane turns into the
// layer's sticky readback exactly as for a single backend.
class ChainedImporter final : public media::GlHwTextureImporter {
 public:
  void Add(std::unique_ptr<media::GlHwTextureImporter> backend) {
    if (backend) {
      backends_.push_back(std::move(backend));
    }
  }

  unsigned int target() const override {
    // The routing backends agree on GL_TEXTURE_2D today (the CGL backend's
    // rectangle target is macOS-only and does not join this chain); if a
    // future backend needs another target, the chain belongs behind a sink
    // that negotiates targets instead of this single-value answer.
    return backends_.empty() ? GL_TEXTURE_2D : backends_.front()->target();
  }

  bool CanImport(const media::VideoFrame& frame) const override {
    for (const auto& backend : backends_) {
      if (backend->CanImport(frame)) {
        return true;
      }
    }
    return false;
  }

  bool ImportPlane(const media::VideoFrame& frame, int index,
                   const media::PlaneUploadLayout& layout) override {
    for (auto& backend : backends_) {
      if (backend->CanImport(frame)) {
        return backend->ImportPlane(frame, index, layout);
      }
    }
    return false;
  }

  const char* name() const override { return "linux-chain"; }

 private:
  std::vector<std::unique_ptr<media::GlHwTextureImporter>> backends_;
};

}  // namespace

std::unique_ptr<media::GlHwTextureImporter> CreateLinuxHwImporter() {
  auto chain = std::make_unique<ChainedImporter>();
#if defined(XMEDIA_ENABLE_EGL_IMPORT)
  chain->Add(CreateEglDmabufImporter());
#endif
#if defined(XMEDIA_ENABLE_CUDA_IMPORT)
  chain->Add(CreateCudaGlImporter());
#endif
  return chain;
}

}  // namespace xmedia::platform::hwimport

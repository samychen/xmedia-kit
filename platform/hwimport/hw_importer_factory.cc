// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/hwimport/hw_importer_factory.h"

#if defined(__APPLE__)
#include "platform/hwimport/cgl_iosurface_importer.h"
#endif

#if defined(__linux__) && defined(XMEDIA_ENABLE_LINUX_IMPORT)
#include "platform/hwimport/linux_hw_importer.h"
#endif

namespace xmedia::platform::hwimport {

std::unique_ptr<media::GlHwTextureImporter> CreateHwTextureImporter() {
#if defined(__APPLE__)
  return CreateCglIosurfaceImporter();
#elif defined(__linux__) && defined(XMEDIA_ENABLE_LINUX_IMPORT)
  // The Linux route: a CHAIN, because this host may carry both zero-copy
  // producer families. VAAPI frames (kVaapiSurface + derived dmabuf plumbing)
  // route to the EGLImage importer; NVDEC frames (kNvdecFrame + CUDA plane
  // plumbing) route to the CUDA-GL interop importer. A GLX-only host fails
  // the first import cleanly and the layer's sticky fallback reads frames
  // back, exactly as it did before importers existed. What the chain must
  // never do is claim UploadPath::kHwZeroCopy for a copy; each backend's own
  // error checks keep that promise.
  return CreateLinuxHwImporter();
#else
  // Windows would be WGL_NV_DX_interop over a D3D11 texture. It is not built,
  // and returning nothing is the honest answer rather than a stub that
  // pretends: the sink reads the frame back on the CPU exactly as it did
  // before, so a hardware frame stays CORRECT on that host -- just not free.
  return nullptr;
#endif
}

}  // namespace xmedia::platform::hwimport

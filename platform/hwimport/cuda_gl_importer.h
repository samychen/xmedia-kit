// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_PLATFORM_HWIMPORT_CUDA_GL_IMPORTER_H_
#define XMEDIA_PLATFORM_HWIMPORT_CUDA_GL_IMPORTER_H_

#include <memory>

#include "media/gl/gl_hw_texture_importer.h"

namespace xmedia::platform::hwimport {

// The NVIDIA half of the zero-copy path: CUDA-GL interop over NVDEC frames.
//
// HOW ONE PLANE GOES IN. The decoder hands over CUdeviceptrs of the NV12
// planes (NvdecFrameInfo, see media/base/video_frame.h). The importer gives
// each plane's GL texture an immutable storage (R8 luma, RG8 interleaved
// chroma -- the same layouts the CGL/EGL backends draw through), registers it
// as a CUDA graphics resource once per texture, and then per frame: push the
// decoder's CUcontext, map, cuMemcpy2D device -> array, unmap. The copy is
// GPU-internal (device memory to the texture backing, no PCIe, no CPU), which
// is what "zero copy" means on this vendor: NVIDIA's GL does not import
// foreign dmabufs (measured: EGLImage bind INVALID_OPERATION on driver 580
// for buffers allocated outside its own allocator; GLX_NV_vdpau_interop is
// not exposed either), so the interop copy IS the supported route.
//
// WHICH CUDA CONTEXT. The one the decoder's hwdevice created, carried on the
// frame and pushed for every operation. THIS TARGET MUST LINK libcuda
// DIRECTLY: dlopening it here produces a second driver instance whose
// handles are invalid for the one libavutil initialized (measured:
// cuCtxPushCurrent returns CUDA_ERROR_INVALID_CONTEXT for every handle). One
// process, one driver instance, two shared users (libavutil decodes, this
// importer displays).
//
// THE TEXTURE LIFETIME. A registered texture's storage is owned by the
// CUDA-GPU pair until unregistered, so the importer keeps the registration
// keyed on the GL texture name and re-registers when the layer hands it a
// different (or resized) texture. Registration happens after the storage is
// defined -- an immutable-storage-less texture is CUDA_ERROR_INVALID_VALUE.
std::unique_ptr<media::GlHwTextureImporter> CreateCudaGlImporter();

}  // namespace xmedia::platform::hwimport

#endif  // XMEDIA_PLATFORM_HWIMPORT_CUDA_GL_IMPORTER_H_

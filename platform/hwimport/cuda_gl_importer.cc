// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/hwimport/cuda_gl_importer.h"

// The CUDA driver API, and its GL-interop half. Vendor SDK headers in
// platform/, same standing as the EGL ones the dmabuf importer includes.
#include <KHR/khrplatform.h>
#include <cuda.h>

#include <EGL/egl.h>

// The one GL enumerant family this file names, from the SAME vendored table
// the caller uses -- glad's. A GL constant spelled twice is this tree's most
// expensive known trap (see GlHwTextureImporter::target()).
#include <glad/gl.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

#include "base/logging.h"

namespace xmedia::platform::hwimport {
namespace {

// Every failure below is reported once and then stays quiet -- the same rule
// the CGL/EGL backends apply. A driver that refuses the interop refuses it
// for every frame, and the layer's sticky fallback turns the first refusal
// into a decision.
void LogOnce(const char* reason) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    LOG(ERROR) << "cuda-gl import unavailable: " << reason;
  }
}

// The CUDA name -> text of the handful of results this path can produce, for
// the one-shot log line. cuGetErrorName would need another entry point; these
// are the ones that matter in practice and the numeric value travels with it
// anyway.
const char* CudaResultName(CUresult r) {
  switch (r) {
  case CUDA_SUCCESS:
    return "ok";
  case CUDA_ERROR_INVALID_VALUE:
    return "invalid-value";
  case CUDA_ERROR_INVALID_HANDLE:
    return "invalid-handle";
  case CUDA_ERROR_NOT_INITIALIZED:
    return "not-initialized";
  case CUDA_ERROR_INVALID_CONTEXT:
    return "invalid-context";
  case CUDA_ERROR_MAP_FAILED:
    return "map-failed";
  case CUDA_ERROR_NOT_MAPPED:
    return "not-mapped";
  default:
    return "cuda-error";
  }
}

// The driver-API GL interop entry point. cudaGL.h declares it, but not every
// CUDA header install exposes the driver-API spelling; declare it by hand
// with the exact ABI the driver exports.
extern "C" CUresult cuGraphicsGLRegisterImage(CUgraphicsResource*, GLuint,
                                              GLenum, unsigned);

// Core GL entry points, resolved the same way the glad table is filled: via
// the dispatch the current context belongs to (glvnd answers these on every
// desktop stack; see the EGL importer's comment for why an unresolvable one
// disqualifies the backend rather than being skipped).
using GlGetErrorFn = unsigned int (*)(void);
using GlGetIntegervFn = void (*)(unsigned int, int*);
GlGetErrorFn gl_get_error_fn() {
  static const GlGetErrorFn fn =
      reinterpret_cast<GlGetErrorFn>(eglGetProcAddress("glGetError"));
  return fn;
}
GlGetIntegervFn gl_get_integerv_fn() {
  static const GlGetIntegervFn fn =
      reinterpret_cast<GlGetIntegervFn>(eglGetProcAddress("glGetIntegerv"));
  return fn;
}

// One registered plane texture. The resource handle is only meaningful while
// |texture| exists and its storage is unchanged, which is why every import
// re-checks identity and size against what the layer handed over.
struct RegisteredTexture {
  GLuint texture{0};
  int width{0};
  int height{0};
  CUgraphicsResource resource{nullptr};
};

class CudaGlImporter final : public media::GlHwTextureImporter {
 public:
  unsigned int target() const override;
  bool CanImport(const media::VideoFrame& frame) const override;
  bool ImportPlane(const media::VideoFrame& frame, int index,
                   const media::PlaneUploadLayout& layout) override;
  const char* name() const override { return "cuda-gl"; }

 private:
  // Registers (or re-registers) |index|'s texture with CUDA. Returns the
  // registered entry, or null with |error| filled when CUDA refused.
  RegisteredTexture* EnsureRegistered(const media::NvdecFrameInfo& info,
                                      int index,
                                      const media::PlaneUploadLayout& layout,
                                      const char** error);

  // Two slots, one per shader plane: 0 = luma R8, 1 = chroma RG8.
  std::array<RegisteredTexture, 2> registered_{};
};

unsigned int CudaGlImporter::target() const {
  return GL_TEXTURE_2D;
}

bool CudaGlImporter::CanImport(const media::VideoFrame& frame) const {
  // NVDEC NV12 with its plumbing attached, the one shape this backend can
  // place. A 10-bit frame never carries the plumbing (the spec's config gate
  // declines those profiles decoder-side), so the format check is the only
  // one the importer needs beyond the pointer.
  return frame.format() == media::VideoFormat::kNV12 &&
         frame.nvdec_info() != nullptr && frame.nvdec_info()->luma != 0 &&
         frame.nvdec_info()->chroma != 0 &&
         frame.nvdec_info()->cuda_ctx != nullptr;
  // DELIBERATELY NO CONTEXT CHECK HERE, same as the CGL backend: the sink
  // asks this before the first frame's context is current. ImportPlane() is
  // where the context must exist, and it is the one that checks.
}

RegisteredTexture*
CudaGlImporter::EnsureRegistered(const media::NvdecFrameInfo& info, int index,
                                 const media::PlaneUploadLayout& layout,
                                 const char** error) {
  RegisteredTexture& entry = registered_[index];
  if (entry.texture == layout.texture_name && entry.width == layout.width &&
      entry.height == layout.height && entry.resource != nullptr) {
    return &entry;
  }
  if (entry.resource != nullptr) {
    // The layer replaced (or resized) this texture: its registration refers
    // to storage that no longer exists.
    cuGraphicsUnregisterResource(entry.resource);
    entry = RegisteredTexture{};
  }

  // Give the texture its storage: CUDA needs an immutable, format-resolved
  // backing to register against, and the layer deliberately allocates none
  // (ImportPlane defines level 0 itself -- see GlVideoLayer::ImportGpuPlanes).
  // The internal formats match the CGL/EGL backends' shader contract: one
  // byte per luma texel, two bytes per chroma texel.
  const GLenum internal = index == 0 ? GL_R8 : GL_RG8;
  using TexStorageFn = void (*)(unsigned int, int, unsigned int, int, int);
  static TexStorageFn tex_storage =
      reinterpret_cast<TexStorageFn>(eglGetProcAddress("glTexStorage2D"));
  if (!tex_storage) {
    *error = "glTexStorage2D not resolvable";
    return nullptr;
  }
  tex_storage(GL_TEXTURE_2D, 1, internal, layout.width, layout.height);
  if (gl_get_error_fn()() != GL_NO_ERROR) {
    *error = "glTexStorage2D failed on the current context";
    return nullptr;
  }

  CUgraphicsResource resource = nullptr;
  const CUresult rc =
      cuGraphicsGLRegisterImage(&resource, layout.texture_name, GL_TEXTURE_2D,
                                CU_GRAPHICS_REGISTER_FLAGS_NONE);
  if (rc != CUDA_SUCCESS) {
    *error = CudaResultName(rc);
    return nullptr;
  }
  entry.texture = layout.texture_name;
  entry.width = layout.width;
  entry.height = layout.height;
  entry.resource = resource;
  return &entry;
}

bool CudaGlImporter::ImportPlane(const media::VideoFrame& frame, int index,
                                 const media::PlaneUploadLayout& layout) {
  const media::NvdecFrameInfo* info = frame.nvdec_info();
  if (info == nullptr || index < 0 || index > 1 || layout.width <= 0 ||
      layout.height <= 0) {
    LogOnce("plane index or layout outside the NV12 pair");
    return false;
  }
  // The layer binds the plane's texture before calling; the current binding
  // is the texture name CUDA registers against. PlaneUploadLayout carries
  // sizes only, so the binding is read here rather than threaded through the
  // public interface.
  GLint bound = 0;
  gl_get_integerv_fn()(GL_TEXTURE_BINDING_2D, &bound);
  const GLuint texture_name = static_cast<GLuint>(bound);
  if (texture_name == 0) {
    LogOnce("the caller bound no texture for this plane");
    return false;
  }

  // Plane geometry: luma w x h from the luma pointer, chroma (w/2) x (h/2)
  // from the chroma pointer, each row taken at the decoder's own stride.
  const uintptr_t src = index == 0 ? info->luma : info->chroma;
  const int32_t src_stride =
      index == 0 ? info->luma_stride : info->chroma_stride;
  const int copy_height = index == 0 ? layout.height : layout.height;
  const size_t copy_width =
      static_cast<size_t>(index == 0 ? layout.width : layout.width * 2);
  if (src == 0 || src_stride <= 0) {
    LogOnce("the frame's plane plumbing is empty");
    return false;
  }

  // Push the decoder's context FIRST: registration and the interop copy are
  // context-relative operations, and without a current context
  // cuGraphicsGLRegisterImage answers CUDA_ERROR_INVALID_CONTEXT (measured).
  // Measured on driver 580, second observation: the handle libavutil created
  // can come back CUDA_ERROR_INVALID_CONTEXT even with the driver linked
  // directly, in which case the device's primary context is pushed instead --
  // the same measurement proved the interop copy content-correct from the
  // primary context for the planes of a decoder context on the same GPU.
  CUresult push_rc = cuCtxPushCurrent(
      static_cast<CUcontext>(const_cast<void*>(info->cuda_ctx)));
  if (push_rc != CUDA_SUCCESS) {
    CUdevice device = 0;
    CUcontext primary = nullptr;
    if (cuDeviceGet(&device, 0) == CUDA_SUCCESS &&
        cuDevicePrimaryCtxRetain(&primary, device) == CUDA_SUCCESS) {
      push_rc = cuCtxPushCurrent(primary);
    }
    if (push_rc != CUDA_SUCCESS) {
      LogOnce(CudaResultName(push_rc));
      return false;
    }
  }

  // NOW the context is current: define the texture's storage (if new) and
  // register it with CUDA, then map + copy + unmap under the same context.
  const char* error = nullptr;
  media::PlaneUploadLayout sized = layout;
  sized.texture_name = texture_name;
  RegisteredTexture* entry = EnsureRegistered(*info, index, sized, &error);
  if (entry == nullptr) {
    cuCtxPopCurrent(nullptr);
    LogOnce(error);
    return false;
  }

  CUresult map_rc = cuGraphicsMapResources(1, &entry->resource, 0);
  CUarray array = nullptr;
  if (map_rc == CUDA_SUCCESS) {
    map_rc = cuGraphicsSubResourceGetMappedArray(&array, entry->resource, 0, 0);
  }
  if (map_rc == CUDA_SUCCESS) {
    CUDA_MEMCPY2D copy = {};
    copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
    copy.srcDevice = static_cast<CUdeviceptr>(src);
    copy.srcPitch = static_cast<size_t>(src_stride);
    copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
    copy.dstArray = array;
    copy.WidthInBytes = copy_width;
    copy.Height = copy_height;
    const CUresult copy_rc = cuMemcpy2D(&copy);
    if (copy_rc != CUDA_SUCCESS) {
      map_rc = copy_rc;
    }
  }
  cuGraphicsUnmapResources(1, &entry->resource, 0);
  cuCtxPopCurrent(nullptr);
  if (map_rc != CUDA_SUCCESS) {
    LogOnce(CudaResultName(map_rc));
    return false;
  }
  return true;
}

}  // namespace

std::unique_ptr<media::GlHwTextureImporter> CreateCudaGlImporter() {
  return std::make_unique<CudaGlImporter>();
}

}  // namespace xmedia::platform::hwimport

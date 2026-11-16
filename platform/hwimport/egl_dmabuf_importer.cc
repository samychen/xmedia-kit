// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/hwimport/egl_dmabuf_importer.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <KHR/khrplatform.h>

// The one GL enumerant this file names, from the SAME vendored table the
// caller uses -- glad's. A GL constant spelled twice is this tree's most
// expensive known trap (see GlHwTextureImporter::target()): the header include
// is what keeps GL_TEXTURE_2D one value on both sides of the seam.
#include <glad/gl.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "base/logging.h"

namespace xmedia::platform::hwimport {
namespace {

// The DRM fourccs an EGL_LINUX_DMA_BUF_EXT import names each plane by. These
// are stable ABI values from drm_fourcc.h (libdrm); spelling them through the
// same fourcc_code packing keeps this file free of a libdrm dependency for
// four integers, and the comment names the canonical source.
constexpr uint32_t FourccCode(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}
constexpr uint32_t kDrmFormatR8 = FourccCode('R', '8', ' ', ' ');
constexpr uint32_t kDrmFormatGr88 = FourccCode('G', 'R', '8', '8');

// Every failure below is reported once and then stays quiet -- the same rule
// the CGL backend applies. A driver that refuses this API refuses it for
// every frame, and the layer's sticky fallback turns the first refusal into
// a decision, so a log line per frame would be a log nobody reads.
void LogOnce(const char* reason) {
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true)) {
    LOG(ERROR) << "egl-dmabuf import unavailable: " << reason;
  }
}

// The extension functions, resolved once. eglGetProcAddress is the right
// source for both: eglCreateImageKHR is an extension entry point, and
// glEGLImageTargetTexture2DOES is an OES GL function that desktop-core
// contexts expose exactly this way.
using EglCreateImageFn = EGLImageKHR(EGLAPIENTRYP)(EGLDisplay, EGLContext,
                                                   EGLenum, EGLClientBuffer,
                                                   const EGLint*);
using EglDestroyImageFn = EGLBoolean(EGLAPIENTRYP)(EGLDisplay, EGLImageKHR);
using GlEglImageTargetTextureFn = void(EGLAPIENTRYP)(unsigned int,
                                                     const void*);

struct EglProcs {
  EglCreateImageFn create_image{nullptr};
  EglDestroyImageFn destroy_image{nullptr};
  GlEglImageTargetTextureFn texture_from_image{nullptr};
  // Core GL entry, but this target links no GL library of its own -- the
  // context came from the host (EGL backend or an EGL-context embedding), so
  // the function is resolved the same way the glad table is filled: through
  // the dispatch the current context belongs to. glvnd answers it on every
  // desktop stack; a stack that does not is one this backend cannot verify a
  // bind on, and an unverified bind is not allowed to count as zero-copy.
  using GlGetErrorFn = unsigned int(EGLAPIENTRYP)(void);
  GlGetErrorFn get_error{nullptr};
};

const EglProcs* EglEntryPoints() {
  static const EglProcs procs = [] {
    EglProcs out;
    out.create_image = reinterpret_cast<EglCreateImageFn>(
        eglGetProcAddress("eglCreateImageKHR"));
    out.destroy_image = reinterpret_cast<EglDestroyImageFn>(
        eglGetProcAddress("eglDestroyImageKHR"));
    out.texture_from_image = reinterpret_cast<GlEglImageTargetTextureFn>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    out.get_error = reinterpret_cast<EglProcs::GlGetErrorFn>(
        eglGetProcAddress("glGetError"));
    return out;
  }();
  return &procs;
}

// Whether the CURRENT display can import dmabufs at all. Asked once per
// display and cached on the pointer: an EGLDisplay is a handle whose value is
// stable for its lifetime, and a display that cannot import will not learn to
// mid-playback. EGL_NO_DISPLAY (a GLX host) fails here with its own reason.
bool DisplayCanImport(EGLDisplay display) {
  struct Cache {
    EGLDisplay display{nullptr};
    bool can{false};
  };
  static Cache cache;
  if (cache.display == display) {
    return cache.can;
  }
  if (display == EGL_NO_DISPLAY) {
    LogOnce("no current EGL display (a GLX context cannot import EGLImages)");
    cache = Cache{display, false};
    return false;
  }
  const char* display_ext = eglQueryString(display, EGL_EXTENSIONS);
  const bool can =
      display_ext != nullptr &&
      // The _mplane variant is the strict superset; either satisfies the
      // single-fd-per-plane import this backend makes.
      (strstr(display_ext, "EGL_EXT_image_dma_buf_import") != nullptr);
  if (!can) {
    LogOnce("current EGL display lacks EGL_EXT_image_dma_buf_import");
  }
  cache = Cache{display, can};
  return can;
}

class EglDmabufImporter final : public media::GlHwTextureImporter {
 public:
  unsigned int target() const override;
  bool CanImport(const media::VideoFrame& frame) const override;
  bool ImportPlane(const media::VideoFrame& frame, int index,
                   const media::PlaneUploadLayout& layout) override;
  const char* name() const override { return "egl-dmabuf"; }
};

unsigned int EglDmabufImporter::target() const {
  // EGLImage binds through GL_TEXTURE_2D -- that is the whole point of the
  // EGL route on Linux, where the rectangle target has no import entry point.
  return GL_TEXTURE_2D;
}

bool EglDmabufImporter::CanImport(const media::VideoFrame& frame) const {
  // The frame must be a native handle with derived dmabuf plumbing. Without
  // the plumbing there is no fd to name, and the answer is "no": the
  // producer either declined the derivation or the build never had it.
  if (frame.dmabuf_info() == nullptr ||
      frame.dmabuf_info()->plane_count <= 0) {
    return false;
  }
  // The 8-bit 4:2:0 layouts only, and the plumbing must actually carry the
  // planes the layout implies -- a 2-plane NV12 info on an I420 frame is a
  // producer bug this gate turns into a readback instead of a bad draw.
  const int expected_planes = frame.format() == media::VideoFormat::kNV12 ? 2
                              : frame.format() == media::VideoFormat::kI420 ? 3
                                                                            : 0;
  if (expected_planes == 0) {
    return false;
  }
  return frame.dmabuf_info()->plane_count == expected_planes;
  // DELIBERATELY NO CONTEXT CHECK HERE, same as the CGL backend: the sink
  // asks this before the first frame's context is current. ImportPlane() is
  // where the context must exist, and it is the one that checks.
}

bool EglDmabufImporter::ImportPlane(const media::VideoFrame& frame, int index,
                                    const media::PlaneUploadLayout& layout) {
  if (layout.width <= 0 || layout.height <= 0) {
    return false;
  }
  const media::DmaBufFrameInfo* info = frame.dmabuf_info();
  if (info == nullptr || index < 0 || index >= info->plane_count) {
    LogOnce("plane index outside the derived dmabuf table");
    return false;
  }
  const EGLDisplay display = eglGetCurrentDisplay();
  if (!DisplayCanImport(display)) {
    // DisplayCanImport logged the reason; a second log line per frame adds
    // nothing.
    return false;
  }
  const EglProcs* procs = EglEntryPoints();
  if (procs->create_image == nullptr || procs->texture_from_image == nullptr ||
      procs->get_error == nullptr) {
    LogOnce("EGL image / GL error entry points not resolvable");
    return false;
  }

  // The import attributes. One fd per PLANE row (a derived NV12 image is one
  // fd, two rows), the PLANE's own size from the caller's layout, and the
  // modifier only when the producer reported one -- omitting the modifier
  // attributes is how a driver is told "linear / unknown", while passing a
  // wrong one is a mismatch the driver may either reject or, worse, honor.
  std::array<EGLint, 17> attrs = {
      EGL_WIDTH,
      layout.width,
      EGL_HEIGHT,
      layout.height,
      EGL_LINUX_DRM_FOURCC_EXT,
      static_cast<EGLint>(index == 1 && frame.format() ==
                                              media::VideoFormat::kNV12
                              ? kDrmFormatGr88
                              : kDrmFormatR8),
      EGL_DMA_BUF_PLANE0_FD_EXT,
      info->planes[index].fd,
      EGL_DMA_BUF_PLANE0_PITCH_EXT,
      static_cast<EGLint>(info->planes[index].stride),
      EGL_DMA_BUF_PLANE0_OFFSET_EXT,
      static_cast<EGLint>(info->planes[index].offset),
      EGL_NONE,
      0,
      0,
      0,
      0};
  int write = 12;  // after EGL_NONE
  if (info->has_modifier) {
    // MODIFIER_LO/HI, then EGL_NONE; the trailing slots absorb the size.
    attrs[write++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
    attrs[write++] = static_cast<EGLint>(info->modifier & 0xFFFFFFFFu);
    attrs[write++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
    attrs[write++] = static_cast<EGLint>(info->modifier >> 32);
  }
  attrs[write] = EGL_NONE;

  const EGLImageKHR image =
      procs->create_image(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                          static_cast<EGLClientBuffer>(nullptr), attrs.data());
  if (image == EGL_NO_IMAGE_KHR) {
    LogOnce("eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT) failed");
    return false;
  }

  // The bound texture's storage becomes the image's. The error check matters
  // more than it looks: a driver can accept the create and refuse the bind
  // (unsupported stride, fd from another device, or a driver that only
  // imports its own allocator's handles -- NVIDIA's desktop GL does exactly
  // that), and an unchecked bind leaves the previous frame's storage in place
  // under a "successful" upload -- the kHwZeroCopy stats claim must not
  // survive that. Drain the error queue FIRST: one glGetError flushes one
  // flag, and a stale error from the presenter's setup would otherwise be
  // mistaken for this bind's verdict.
  while (procs->get_error() != GL_NO_ERROR) {
  }
  procs->texture_from_image(GL_TEXTURE_2D, image);
  const unsigned int gl_error = procs->get_error();
  // The image handle is dead weight after the bind either way; on success the
  // texture holds the storage reference, on failure holding it would leak.
  procs->destroy_image(display, image);
  if (gl_error != GL_NO_ERROR) {
    LogOnce("glEGLImageTargetTexture2DOES failed on the current context");
    return false;
  }
  return true;
}

}  // namespace

std::unique_ptr<media::GlHwTextureImporter> CreateEglDmabufImporter() {
  return std::make_unique<EglDmabufImporter>();
}

}  // namespace xmedia::platform::hwimport

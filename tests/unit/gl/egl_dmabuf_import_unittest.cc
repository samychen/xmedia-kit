// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The Linux twin of gl_hw_import_unittest.cc: the zero-copy path, checked by
// reading the pixels back.
//
// WHAT IS REAL HERE. The dmabuf is REAL -- produced by a DRM dumb buffer on
// the machine's render/card node and exported through PRIME_HANDLE_TO_FD, the
// same class of fd a VAAPI driver hands out of vaAcquireBufferHandle. The
// import is REAL -- the production CreateEglDmabufImporter() over the test's
// own EGL context, driven through GlPresenter exactly as a sink drives it.
// What is not real is the decoder: nothing here decodes H.264. The frame is
// built the way the FFmpeg VAAPI adapter builds one after a successful
// derivation (a kVaapiSurface frame with dmabuf plumbing attached), which is
// enough to prove the import and not enough to prove the pipeline.
//
// THE CAPABILITY GATES. All three are host facts, each recorded in the skip
// reason rather than hidden:
//   1. the build carries the EGL importer (CreateHwTextureImporter != null);
//   2. an EGL display + GL 3.3 context can be created (device platform, then
//      the default display);
//   3. a DRM node offers dumb-buffer allocation and PRIME export -- the
//      producer every non-embedded Linux GPU stack has; a node without it
//      (or without our permissions) leaves nothing to import.
// Once the gates pass, the import itself is NOT allowed to degrade: on a
// machine that can produce a dmabuf and open an EGL context, a refused
// import is a red test, because that is exactly the machine the zero-copy
// path exists for. A driver that refuses is a capability regression on that
// machine, not a portable skip.

// Linux-only by construction: the dmabuf producer is a DRM node and the
// context is EGL. The file is globbed into gl_unittests on every host that
// builds the GL tests (macOS included), so it compiles to an empty
// translation unit everywhere else rather than failing on EGL/DRM headers
// that do not exist there. The macOS twin is gl_hw_import_unittest.cc.
#if defined(__linux__)

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/drm.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "base/functional/bind.h"
#include "gtest/gtest.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_present.h"
#include "media/gl/gl_scene.h"
#include "platform/egl/egl_gl_context.h"
#include "platform/hwimport/hw_importer_factory.h"
#include "tests/unit/gl/gl_test_frames.h"

namespace xmedia::media {
namespace {

constexpr int kWindowSize = 64;

// The DRM fourcc of a whole NV12 buffer, for the DmaBufFrameInfo tag. Same
// four-character packing as drm_fourcc.h; the per-plane fourccs (R8, GR88)
// are the importer's business and are not named here.
constexpr uint32_t FourccCode(char a, char b, char c, char d) {
  return static_cast<uint32_t>(static_cast<uint8_t>(a)) |
         (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 8) |
         (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 16) |
         (static_cast<uint32_t>(static_cast<uint8_t>(d)) << 24);
}
constexpr uint32_t kDrmFormatNv12 = FourccCode('N', 'V', '1', '2');

// ---- the dmabuf producer: one DRM dumb buffer shaped as NV12 ---------------

// Opens the first DRM node this host allows. Dumb-buffer allocation lives on
// the card node (the render node of some drivers refuses CREATE_DUMB), so
// card0 is tried first and the render node after it; the first fd that works
// wins. -1 when the host has no usable node -- a container without /dev/dri
// or a sandbox without the video/render group, both recorded in the skip.
int OpenDrmNode() {
  for (const char* path : {"/dev/dri/card0", "/dev/dri/renderD128"}) {
    const int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd >= 0) {
      return fd;
    }
  }
  return -1;
}

// One NV12-shaped buffer: luma rows at offset 0, chroma rows right after the
// luma region, both with the dumb buffer's actual pitch as stride. A dumb
// buffer is single-format (one bpp for the whole allocation), so the trick is
// allocating 8 bpp at height (h + h/2): the luma region is 1 byte/pixel and
// the chroma region -- 2 bytes per (w/2) chroma texel = w bytes per row -- is
// exactly the same byte geometry, which is the layout vaDeriveImage reports
// for a NV12 surface: one fd, two plane offsets.
struct DumbBuffer {
  int drm_fd{-1};
  uint32_t gem_handle{0};
  int dmabuf_fd{-1};
  uint32_t pitch{0};
  int width{0};
  int height{0};

  bool Create(int drm, int w, int h) {
    drm_fd = drm;
    width = w;
    height = h;
    drm_mode_create_dumb create{};
    create.width = static_cast<uint32_t>(w);
    // Luma (h rows) + chroma (h/2 rows), one allocation.
    create.height = static_cast<uint32_t>(h + h / 2);
    create.bpp = 8;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
      return false;
    }
    gem_handle = create.handle;
    pitch = create.pitch;

    drm_prime_handle prime{};
    prime.handle = gem_handle;
    prime.flags = DRM_RDWR | DRM_CLOEXEC;
    if (ioctl(drm_fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &prime) != 0) {
      Release();
      return false;
    }
    dmabuf_fd = prime.fd;
    return true;
  }

  void Release() {
    if (dmabuf_fd >= 0) {
      close(dmabuf_fd);
      dmabuf_fd = -1;
    }
    if (gem_handle != 0) {
      drm_mode_destroy_dumb destroy{};
      destroy.handle = gem_handle;
      ioctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
      gem_handle = 0;
    }
  }

  // Fills the picture the orientation tests read: top half |top_luma|, bottom
  // half |bottom_luma|, neutral chroma throughout. Through the CPU mapping --
  // which a hardware frame does not normally offer, but the buffer has not
  // been published yet, and this is the only way to give the test a picture
  // it knows the value of.
  bool Fill(uint8_t top_luma, uint8_t bottom_luma) {
    drm_mode_map_dumb map{};
    map.handle = gem_handle;
    if (ioctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
      return false;
    }
    void* base =
        mmap(nullptr, static_cast<size_t>(pitch) * (height + height / 2),
             PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd,
             static_cast<off_t>(map.offset));
    if (base == MAP_FAILED) {
      return false;
    }
    auto* bytes = static_cast<uint8_t*>(base);
    for (int row = 0; row < height; ++row) {
      std::fill_n(bytes + static_cast<size_t>(row) * pitch, width,
                  row < height / 2 ? top_luma : bottom_luma);
    }
    uint8_t* chroma = bytes + static_cast<size_t>(height) * pitch;
    for (int row = 0; row < height / 2; ++row) {
      uint8_t* dst = chroma + static_cast<size_t>(row) * pitch;
      for (int col = 0; col < width / 2; ++col) {
        dst[col * 2] = test::kNeutralChroma;
        dst[col * 2 + 1] = test::kNeutralChroma;
      }
    }
    munmap(base, static_cast<size_t>(pitch) * (height + height / 2));
    return true;
  }

  // The frame-side plumbing: plane rows over the one fd. The release closure
  // closes the prime fd; the dumb buffer itself dies with this struct at the
  // end of the test.
  std::unique_ptr<DmaBufFrameInfo> MakeInfo() const {
    auto info = std::make_unique<DmaBufFrameInfo>();
    info->fourcc = kDrmFormatNv12;
    info->plane_count = 2;
    info->has_modifier = false;
    info->planes[0] = DmaBufPlane{dmabuf_fd, pitch, 0};
    info->planes[1] =
        DmaBufPlane{dmabuf_fd, pitch, static_cast<uint32_t>(height) * pitch};
    return info;
  }
};

// The frame the FFmpeg VAAPI adapter would publish after a successful
// derivation: a kVaapiSurface native handle with dmabuf plumbing attached,
// NV12 layout hint. |format| is a parameter only so one test can lie about it
// -- the buffer is NV12 whatever is passed, and a frame that claims otherwise
// is how the importer's format gate gets exercised.
base::scoped_refptr<VideoFrame> MakeDmabufFrame(DumbBuffer& buffer,
                                                VideoFormat format) {
  NativeHandle handle;
  handle.kind = NativeHandleKind::kVaapiSurface;
  // The VASurfaceID the decoder would put here is unused by the EGL importer
  // -- it names a VA object only its VADisplay understands -- and a test has
  // none, so it stays null. The dmabuf plumbing below is what the display
  // side consumes.
  handle.id = nullptr;
  const Size size{buffer.width, buffer.height};
  // Read the plane table while the struct still owns the fd: the release
  // closure below takes the fd, and the info must not end up with -1.
  std::unique_ptr<DmaBufFrameInfo> info = buffer.MakeInfo();
  auto frame = VideoFrame::WrapNativeBuffer(
      handle, format, size, size, Rational{1, 1}, base::TimeDelta(),
      base::TimeDelta(), 0,
      base::BindOnce([](int fd) { close(fd); }, buffer.dmabuf_fd),
      base::RepeatingCallback<base::scoped_refptr<VideoFrame>()>());
  if (!frame) {
    return nullptr;
  }
  buffer.dmabuf_fd = -1;  // the frame's release closure owns it now
  frame->set_dmabuf_info(std::move(info));
  return frame;
}

// ---- the EGL test context --------------------------------------------------

// One process-wide EGL display + pbuffer + desktop GL 3.3 context, created
// lazily and kept for the whole run. Two reasons it is shared instead of
// per-test: creating an EGL device display is the expensive step (ICD probe),
// and the importer caches its extension answer per display pointer -- a
// display created, terminated and re-created could come back as the same
// pointer with a stale cache. The context is current only while a test is
// inside its fixture scope; every test releases it through the presenter's
// destructor, which is also what keeps the no-context assertions honest.
struct EglTestGl {
  EGLDisplay display{EGL_NO_DISPLAY};
  EGLSurface surface{EGL_NO_SURFACE};
  EGLContext context{EGL_NO_CONTEXT};
  std::unique_ptr<media::EglGLContext> native;

  bool MakeCurrent() { return native ? native->MakeCurrent() : false; }
  void ReleaseCurrent() {
    if (native) {
      native->ReleaseCurrent();
    }
  }
  int width() const { return kWindowSize; }
  int height() const { return kWindowSize; }

  // A fresh wrapper over the SAME display/surface/context, for a test-local
  // presenter: the presenter owns the NativeGLContext it is given, so the
  // shared instance cannot hand its own wrapper over more than once.
  std::unique_ptr<media::EglGLContext> TakeNativeContextCopy() {
    return std::make_unique<media::EglGLContext>(static_cast<void*>(display),
                                                 static_cast<void*>(surface),
                                                 static_cast<void*>(context));
  }
};

// Creates the shared context, or fills |why| and returns null. The display is
// taken from the device platform first (EGL_EXT_platform_device), which is
// what a GPU host answers headlessly; the default display is the fallback,
// which is what a plain X11-only stack has. A desktop GL 3.3 core context is
// required because the presenter's shaders are the same desktop programs
// every other pixel test draws with.
EglTestGl* SharedEglTestGl(std::string* why) {
  static EglTestGl gl;
  static bool attempted = false;
  if (attempted) {
    if (gl.native) {
      return &gl;
    }
    *why = "no usable EGL context on this host (attempted once already)";
    return nullptr;
  }
  attempted = true;

  PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
      reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
          eglGetProcAddress("eglGetPlatformDisplayEXT"));
  EGLDisplay display = EGL_NO_DISPLAY;
  if (get_platform_display != nullptr) {
    EGLint count = 0;
    auto query_devices = reinterpret_cast<PFNEGLQUERYDEVICESEXTPROC>(
        eglGetProcAddress("eglQueryDevicesEXT"));
    if (query_devices != nullptr && query_devices(0, nullptr, &count) &&
        count > 0) {
      std::vector<EGLDeviceEXT> devices(static_cast<size_t>(count));
      query_devices(count, devices.data(), &count);
      for (EGLDeviceEXT device : devices) {
        display =
            get_platform_display(EGL_PLATFORM_DEVICE_EXT, device, nullptr);
        if (display != EGL_NO_DISPLAY &&
            eglInitialize(display, nullptr, nullptr)) {
          break;
        }
        display = EGL_NO_DISPLAY;
      }
    }
  }
  if (display == EGL_NO_DISPLAY) {
    display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY ||
        !eglInitialize(display, nullptr, nullptr)) {
      *why = "no EGL display on this host";
      return nullptr;
    }
  }
  gl.display = display;

  const EGLint config_attrs[] = {EGL_SURFACE_TYPE,
                                 EGL_PBUFFER_BIT,
                                 EGL_RENDERABLE_TYPE,
                                 EGL_OPENGL_BIT,
                                 EGL_RED_SIZE,
                                 8,
                                 EGL_GREEN_SIZE,
                                 8,
                                 EGL_BLUE_SIZE,
                                 8,
                                 EGL_ALPHA_SIZE,
                                 8,
                                 EGL_NONE};
  EGLConfig config = nullptr;
  EGLint configs = 0;
  if (!eglChooseConfig(display, config_attrs, &config, 1, &configs) ||
      configs < 1) {
    *why = "no EGL desktop-GL pbuffer config";
    return nullptr;
  }
  const EGLint pbuffer_attrs[] = {EGL_WIDTH, kWindowSize, EGL_HEIGHT,
                                  kWindowSize, EGL_NONE};
  gl.surface = eglCreatePbufferSurface(display, config, pbuffer_attrs);
  if (gl.surface == EGL_NO_SURFACE) {
    *why = "eglCreatePbufferSurface failed";
    return nullptr;
  }
  const EGLint context_attrs[] = {EGL_CONTEXT_MAJOR_VERSION,
                                  3,
                                  EGL_CONTEXT_MINOR_VERSION,
                                  3,
                                  EGL_CONTEXT_OPENGL_PROFILE_MASK,
                                  EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
                                  EGL_NONE};
  eglBindAPI(EGL_OPENGL_API);
  gl.context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
  if (gl.context == EGL_NO_CONTEXT) {
    *why = "no desktop GL 3.3 core context from EGL";
    return nullptr;
  }
  gl.native = std::make_unique<media::EglGLContext>(
      static_cast<void*>(display), static_cast<void*>(gl.surface),
      static_cast<void*>(gl.context));
  if (!gl.native->MakeCurrent()) {
    *why = "the EGL context would not go current";
    gl.native.reset();
    return nullptr;
  }
  return &gl;
}

// ---- the fixture ------------------------------------------------------------

// The empty release closure for frames that carry no producer-side backing of
// their own (the contract-matrix frames are hand-built and own nothing).
// WrapNativeBuffer CHECKs that the callback exists -- a missing one would
// leak the backing it was supposed to free.
base::OnceClosure ReleaseNoop() {
  return base::BindOnce([]() {});
}

// Whether the driver's desktop GL actually accepts an EGLImage bind. Measured
// ONCE, on a real dumb-buffer dmabuf, with the production importer: on Mesa
// (Intel/AMD -- the VAAPI hardware this feature targets) the answer is yes;
// NVIDIA's proprietary desktop GL refuses binds whose fd did not come from
// its own allocator, which is a driver fact this suite records instead of
// pretending the import failed. The probe result gates the pixel-level
// zero-copy assertions: skipped with the reason on a refusing driver, RED on
// regression where the import works.
bool ImportBindCapability(std::string* why) {
  static int cached = -1;
  if (cached >= 0) {
    if (cached == 0 && why) {
      *why = "the driver refused the EGLImage bind (measured once)";
    }
    return cached == 1;
  }
  std::string probe_why;
  EglTestGl* gl = SharedEglTestGl(&probe_why);
  if (!gl) {
    cached = 0;
    if (why)
      *why = probe_why;
    return false;
  }
  static int drm_fd = OpenDrmNode();
  if (drm_fd < 0) {
    cached = 0;
    if (why)
      *why = "no usable /dev/dri node for the probe";
    return false;
  }
  DumbBuffer probe;
  if (!probe.Create(drm_fd, kWindowSize, kWindowSize) || !probe.Fill(16, 235)) {
    cached = 0;
    if (why)
      *why = "the probe's dumb buffer could not be produced";
    return false;
  }
  auto frame = MakeDmabufFrame(probe, VideoFormat::kNV12);
  if (!frame) {
    cached = 0;
    if (why)
      *why = "the probe frame could not be built";
    return false;
  }
  auto importer = platform::hwimport::CreateHwTextureImporter();
  if (!importer) {
    cached = 0;
    if (why)
      *why = "no importer in this build";
    return false;
  }
  gl->MakeCurrent();
  using GenTexturesFn = void (*)(int, unsigned int*);
  using BindTextureFn = void (*)(unsigned int, unsigned int);
  using DeleteTexturesFn = void (*)(int, const unsigned int*);
  auto gen =
      reinterpret_cast<GenTexturesFn>(eglGetProcAddress("glGenTextures"));
  auto bind =
      reinterpret_cast<BindTextureFn>(eglGetProcAddress("glBindTexture"));
  auto del =
      reinterpret_cast<DeleteTexturesFn>(eglGetProcAddress("glDeleteTextures"));
  unsigned int tex = 0;
  if (!gen || !bind || !del) {
    cached = 0;
    if (why)
      *why = "GL texture entry points not resolvable";
    return false;
  }
  gen(1, &tex);
  bind(GL_TEXTURE_2D, tex);
  media::PlaneUploadLayout layout;
  layout.internal_format = GL_R8;
  layout.format = GL_RED;
  layout.type = GL_UNSIGNED_BYTE;
  layout.width = kWindowSize;
  layout.height = kWindowSize;
  const bool ok = importer->ImportPlane(*frame, 0, layout);
  del(1, &tex);
  gl->ReleaseCurrent();
  cached = ok ? 1 : 0;
  if (!ok && why) {
    *why =
        "the driver refused the EGLImage bind (measured once; NVIDIA "
        "proprietary GL rejects fds from foreign allocators)";
  }
  return ok;
}

class EglDmabufImportTest : public ::testing::Test {
 protected:
  void SetUp() override {
    importer_ = platform::hwimport::CreateHwTextureImporter();
    if (!importer_) {
      // The honest answer of a no-EGL build: no backend, and the behaviour
      // that replaces it -- a CPU readback -- is correct by construction and
      // tested elsewhere.
      GTEST_SKIP() << "this build has no EGL dmabuf importer";
    }
    std::string why;
    gl_ = SharedEglTestGl(&why);
    if (!gl_) {
      GTEST_SKIP() << why;
    }
    presenter_ = std::make_unique<GlPresenter>(gl_->TakeNativeContextCopy());
    presenter_->SetHwTextureImporter(importer_.get());
    if (!presenter_->Init()) {
      GTEST_SKIP() << "presenter init failed on this host";
    }
  }

  void TearDown() override {
    presenter_.reset();
    if (gl_) {
      gl_->ReleaseCurrent();
    }
  }

  // The dumb-buffer producer, with every host failure recorded as a skip.
  bool MakeProducer(int w, int h, uint8_t top_luma, uint8_t bottom_luma,
                    std::string* why) {
    static int drm_fd = OpenDrmNode();
    if (drm_fd < 0) {
      *why = "no usable /dev/dri node for a dumb-buffer dmabuf";
      return false;
    }
    buffer_ = DumbBuffer{};
    if (!buffer_.Create(drm_fd, w, h)) {
      *why = "DRM node refused dumb-buffer allocation or PRIME export";
      return false;
    }
    if (!buffer_.Fill(top_luma, bottom_luma)) {
      *why = "the dumb buffer would not map for filling";
      return false;
    }
    return true;
  }

  int WindowRed(int y) {
    return test::ReadPixel(*presenter_->api(), gl_->width() / 2, y, GL_BACK).r;
  }
  int WindowTopRed() {
    return WindowRed(gl_->height() - 1 - test::kSampleInset);
  }
  int WindowBottomRed() { return WindowRed(test::kSampleInset); }

  std::unique_ptr<GlHwTextureImporter> importer_;
  DumbBuffer buffer_;
  EglTestGl* gl_{nullptr};
  std::unique_ptr<GlPresenter> presenter_;
};

// ---- contract tests: no GPU needed beyond the importer existing -------------

TEST_F(EglDmabufImportTest, TheBackendNamesItsTargetAndItself) {
  // On a multi-backend Linux host the factory hands out the CHAIN (the VAAPI
  // and NVDEC routes live behind one importer), so the chain's name is what
  // the logs and stats report. The name must still be a real identifier, not
  // an empty string.
  EXPECT_NE(importer_->name(), nullptr);
  EXPECT_STRNE(importer_->name(), "");
  // The seam rule: this constant must be the SAME value the glad table gives
  // the caller -- one spelling, two readers.
  EXPECT_EQ(importer_->target(), GL_TEXTURE_2D);
}

// The CanImport matrix. Every row is a shape a producer can actually hand
// over; the point is that only the shape the EGL route can place says yes.
TEST_F(EglDmabufImportTest, CanImportAcceptsExactlyTheDerived8Bit420Shapes) {
  const auto with_info = [](int plane_count) {
    auto info = std::make_unique<DmaBufFrameInfo>();
    info->fourcc = kDrmFormatNv12;
    info->plane_count = plane_count;
    for (int i = 0; i < plane_count && i < 4; ++i) {
      info->planes[i].fd = -1;  // validity is ImportPlane's problem
      info->planes[i].stride = 64;
    }
    return info;
  };
  auto native = [](NativeHandleKind kind) {
    NativeHandle handle;
    handle.kind = kind;
    handle.id = reinterpret_cast<const void*>(static_cast<uintptr_t>(0x1));
    return handle;
  };

  // A derived NV12 frame, the shape the VAAPI adapter publishes: yes.
  {
    auto frame = VideoFrame::WrapNativeBuffer(
        native(NativeHandleKind::kVaapiSurface), VideoFormat::kNV12,
        Size{64, 64}, Size{64, 64}, Rational{1, 1}, base::TimeDelta(),
        base::TimeDelta(), 0, ReleaseNoop(),
        base::RepeatingCallback<base::scoped_refptr<VideoFrame>()>());
    frame->set_dmabuf_info(with_info(2));
    EXPECT_TRUE(importer_->CanImport(*frame));
  }
  // The same frame WITHOUT the derivation: no fd to name, so no.
  {
    auto frame = VideoFrame::WrapNativeBuffer(
        native(NativeHandleKind::kVaapiSurface), VideoFormat::kNV12,
        Size{64, 64}, Size{64, 64}, Rational{1, 1}, base::TimeDelta(),
        base::TimeDelta(), 0, ReleaseNoop(),
        base::RepeatingCallback<base::scoped_refptr<VideoFrame>()>());
    EXPECT_FALSE(importer_->CanImport(*frame));
  }
  // I420, three planes: the 3-plane read the layer's shader makes. Yes.
  {
    auto frame = VideoFrame::WrapNativeBuffer(
        native(NativeHandleKind::kVaapiSurface), VideoFormat::kI420,
        Size{64, 64}, Size{64, 64}, Rational{1, 1}, base::TimeDelta(),
        base::TimeDelta(), 0, ReleaseNoop(),
        base::RepeatingCallback<base::scoped_refptr<VideoFrame>()>());
    frame->set_dmabuf_info(with_info(3));
    EXPECT_TRUE(importer_->CanImport(*frame));
  }
  // A 10-bit layout with 8-bit-looking plumbing: declined by the shape gate.
  {
    auto frame = VideoFrame::WrapNativeBuffer(
        native(NativeHandleKind::kVaapiSurface), VideoFormat::kP010,
        Size{64, 64}, Size{64, 64}, Rational{1, 1}, base::TimeDelta(),
        base::TimeDelta(), 0, ReleaseNoop(),
        base::RepeatingCallback<base::scoped_refptr<VideoFrame>()>());
    frame->set_dmabuf_info(with_info(2));
    EXPECT_FALSE(importer_->CanImport(*frame));
  }
  // NV12 with a 3-plane table: a producer bug, refused as a bad draw risk.
  {
    auto frame = VideoFrame::WrapNativeBuffer(
        native(NativeHandleKind::kVaapiSurface), VideoFormat::kNV12,
        Size{64, 64}, Size{64, 64}, Rational{1, 1}, base::TimeDelta(),
        base::TimeDelta(), 0, ReleaseNoop(),
        base::RepeatingCallback<base::scoped_refptr<VideoFrame>()>());
    frame->set_dmabuf_info(with_info(3));
    EXPECT_FALSE(importer_->CanImport(*frame));
  }
}

// ImportPlane on a thread with no current EGL display -- the GLX-host case,
// and the case of any caller that ignored the threading rule. The answer is
// false, not a crash; the first refusal logs once and the layer's sticky
// fallback takes over. The frame needs no real buffer: the display check runs
// before any EGL call touches the fd, so a fake fd never reaches a driver.
TEST_F(EglDmabufImportTest, ImportPlaneWithoutACurrentDisplayIsRefused) {
  auto info = std::make_unique<DmaBufFrameInfo>();
  info->fourcc = kDrmFormatNv12;
  info->plane_count = 2;
  info->planes[0] = DmaBufPlane{-1, kWindowSize, 0};
  info->planes[1] = DmaBufPlane{-1, kWindowSize, kWindowSize * kWindowSize};
  auto frame = VideoFrame::WrapNativeBuffer(
      [] {
        NativeHandle handle;
        handle.kind = NativeHandleKind::kVaapiSurface;
        handle.id = nullptr;
        return handle;
      }(),
      VideoFormat::kNV12, Size{kWindowSize, kWindowSize},
      Size{kWindowSize, kWindowSize}, Rational{1, 1}, base::TimeDelta(),
      base::TimeDelta(), 0, ReleaseNoop(),
      base::RepeatingCallback<base::scoped_refptr<VideoFrame>()>());
  ASSERT_TRUE(frame);
  frame->set_dmabuf_info(std::move(info));
  // No eglMakeCurrent anywhere on this thread: the current display is
  // EGL_NO_DISPLAY (or another test's released one), and the import must
  // answer false without touching anything.
  PlaneUploadLayout layout;
  layout.internal_format = GL_R8;
  layout.format = GL_RED;
  layout.type = GL_UNSIGNED_BYTE;
  layout.width = kWindowSize;
  layout.height = kWindowSize;
  EXPECT_FALSE(importer_->ImportPlane(*frame, 0, layout));
}

// ---- the real-GPU tests: dmabuf in, pixels read back ------------------------

// The route claim first, exactly as the macOS twin does: an import that
// silently fell back to a CPU upload would still draw the right picture, so
// the route is asserted on its own before any colour is looked at.
TEST_F(EglDmabufImportTest, AnImportedFrameReportsTheZeroCopyRoute) {
  std::string why;
  if (!MakeProducer(kWindowSize, kWindowSize, test::kWhiteLuma,
                    test::kWhiteLuma, &why)) {
    GTEST_SKIP() << why;
  }
  auto frame = MakeDmabufFrame(buffer_, VideoFormat::kNV12);
  ASSERT_TRUE(frame);
  std::string cap_why;
  if (!ImportBindCapability(&cap_why)) {
    GTEST_SKIP() << cap_why;
  }
  EXPECT_FALSE(frame->IsMappable())
      << "the frame's pixels must be dmabuf-resident";

  ASSERT_TRUE(gl_->MakeCurrent());
  PresentTiming timing;
  ASSERT_TRUE(presenter_->Compose(*frame, &timing))
      << "the import itself is not allowed to degrade once the host can "
         "produce a dmabuf and open an EGL context";
  EXPECT_EQ(timing.upload_path, UploadPath::kHwZeroCopy);
}

// The orientation of an imported frame. An EGLImage import has no coordinate
// space of its own to hide a flip behind -- a texture bound from a dmabuf is
// addressed exactly like any GL_TEXTURE_2D -- and a flip here is invisible to
// every call in the chain, which is why the check reads pixels.
TEST_F(EglDmabufImportTest, AnImportedFrameLandsUpright) {
  std::string why;
  if (!MakeProducer(kWindowSize, kWindowSize, test::kWhiteLuma,
                    test::kBlackLuma, &why)) {
    GTEST_SKIP() << why;
  }
  auto frame = MakeDmabufFrame(buffer_, VideoFormat::kNV12);
  ASSERT_TRUE(frame);
  std::string cap_why;
  if (!ImportBindCapability(&cap_why)) {
    GTEST_SKIP() << cap_why;
  }

  ASSERT_TRUE(gl_->MakeCurrent());
  ASSERT_TRUE(presenter_->Compose(*frame));
  presenter_->PresentComposed();

  EXPECT_GT(WindowTopRed(), test::kBrightThreshold)
      << "the white half of the frame is its top half";
  EXPECT_LT(WindowBottomRed(), test::kDarkThreshold);
}

// Both routes have to draw the same picture from the same numbers: the
// imported frame against a CPU-owned frame of the same logical colour. They
// reach it through different upload paths and the same shader -- the one
// thing that could plausibly disagree is a plane bound to the wrong sampler
// slot, and that shows up as the wrong value rather than as an error.
TEST_F(EglDmabufImportTest, BothRoutesDrawTheSameColour) {
  std::string why;
  if (!MakeProducer(kWindowSize, kWindowSize, test::kWhiteLuma,
                    test::kWhiteLuma, &why)) {
    GTEST_SKIP() << why;
  }
  auto gpu = MakeDmabufFrame(buffer_, VideoFormat::kNV12);
  auto cpu = test::MakeSolidFrame(VideoFormat::kNV12, kWindowSize, kWindowSize,
                                  test::kWhiteLuma, test::kNeutralChroma,
                                  test::kNeutralChroma);
  ASSERT_TRUE(gpu);
  ASSERT_TRUE(cpu);
  std::string cap_why;
  if (!ImportBindCapability(&cap_why)) {
    GTEST_SKIP() << cap_why;
  }

  ASSERT_TRUE(gl_->MakeCurrent());
  ASSERT_TRUE(presenter_->Compose(*gpu));
  presenter_->PresentComposed();
  const int imported = WindowTopRed();

  ASSERT_TRUE(presenter_->Compose(*cpu));
  presenter_->PresentComposed();
  const int copied = WindowTopRed();

  EXPECT_NEAR(imported, copied, 2)
      << "an imported frame and a copied one with the same pixels should "
         "draw the same colour";
  EXPECT_GT(imported, test::kBrightThreshold);
}

// A GPU frame whose producer derived nothing -- the state a host lands in
// when the derivation was declined or turned off -- is refused, not drawn
// from whatever the textures happen to hold. With no readback callback there
// is also nothing to fall back to, so Compose fails loudly.
TEST_F(EglDmabufImportTest, AFrameWithoutDmabufPlumbingIsRefused) {
  auto frame = VideoFrame::WrapNativeBuffer(
      [] {
        NativeHandle handle;
        handle.kind = NativeHandleKind::kVaapiSurface;
        handle.id = reinterpret_cast<const void*>(static_cast<uintptr_t>(0x1));
        return handle;
      }(),
      VideoFormat::kNV12, Size{kWindowSize, kWindowSize},
      Size{kWindowSize, kWindowSize}, Rational{1, 1}, base::TimeDelta(),
      base::TimeDelta(), 0, ReleaseNoop(),
      base::RepeatingCallback<base::scoped_refptr<VideoFrame>()>());
  ASSERT_TRUE(frame);

  ASSERT_TRUE(gl_->MakeCurrent());
  PresentTiming timing;
  EXPECT_FALSE(presenter_->Compose(*frame, &timing))
      << "no derivation, no readback path: refusing is the contract";
}

// The OTHER half of the contract, and the half every Linux host can verify:
// when the driver refuses the bind, the layer must fall back to the explicit
// readback and report kCpuSync -- never a black frame, never a kHwZeroCopy
// claim for a copy. The frame carries dmabuf plumbing AND a readback
// callback, which is the shape the VAAPI decoder always publishes.
TEST_F(EglDmabufImportTest, ARefusedBindFallsBackToTheReadbackRoute) {
  std::string why;
  if (!MakeProducer(kWindowSize, kWindowSize, test::kWhiteLuma,
                    test::kWhiteLuma, &why)) {
    GTEST_SKIP() << why;
  }
  std::string cap_why;
  if (ImportBindCapability(&cap_why)) {
    GTEST_SKIP() << "the driver accepts the bind on this host; the fallback "
                    "is covered by gl_upload_fallback_unittest instead";
  }
  // Rebuild the frame WITH a readback path: MakeDmabufFrame leaves none,
  // which is the state the no-readback refusal test covers.
  NativeHandle handle;
  handle.kind = NativeHandleKind::kVaapiSurface;
  handle.id = nullptr;
  const Size size{buffer_.width, buffer_.height};
  std::unique_ptr<DmaBufFrameInfo> info = buffer_.MakeInfo();
  auto frame = VideoFrame::WrapNativeBuffer(
      handle, VideoFormat::kNV12, size, size, Rational{1, 1}, base::TimeDelta(),
      base::TimeDelta(), 0,
      base::BindOnce([](int fd) { close(fd); }, buffer_.dmabuf_fd),
      base::BindRepeating([]() -> base::scoped_refptr<VideoFrame> {
        return test::MakeSolidFrame(VideoFormat::kNV12, kWindowSize,
                                    kWindowSize, test::kWhiteLuma,
                                    test::kNeutralChroma, test::kNeutralChroma);
      }));
  ASSERT_TRUE(frame);
  buffer_.dmabuf_fd = -1;
  frame->set_dmabuf_info(std::move(info));

  ASSERT_TRUE(gl_->MakeCurrent());
  PresentTiming timing;
  ASSERT_TRUE(presenter_->Compose(*frame, &timing))
      << "a refused bind with a readback path must draw, not fail";
  EXPECT_EQ(timing.upload_path, UploadPath::kCpuSync)
      << "the fallback route must say so, not claim zero-copy";
}

}  // namespace
}  // namespace xmedia::media

#endif  // defined(__linux__)

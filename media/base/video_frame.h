// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// API mirrors Chromium's `media/base/video_frame.h` (BSD-3-Clause).
//
// Replaces the two parallel hierarchies legacy maintains for the same
// concept: `AVFrame` (software pixels) and `SDL_VoutOverlay` (hardware
// surfaces). Chromium never needed the second one because VideoFrame carries
// a StorageType; see docs/02 §9 D2.

#ifndef XMEDIA_MEDIA_BASE_VIDEO_FRAME_H_
#define XMEDIA_MEDIA_BASE_VIDEO_FRAME_H_

#include <stdint.h>

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string>

#include "base/functional/callback.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/video_color_space.h"
#include "media/media_export.h"

namespace xmedia::media {

struct XMEDIA_MEDIA_EXPORT Rational {
  int num{1};
  int den{1};
  constexpr bool is_zero() const noexcept { return num == 0; }
  constexpr double ToDouble() const noexcept {
    return den == 0 ? 0.0 : static_cast<double>(num) / static_cast<double>(den);
  }
  constexpr friend bool operator==(const Rational&, const Rational&) = default;
};

struct XMEDIA_MEDIA_EXPORT Size {
  int width{0};
  int height{0};
  constexpr int GetArea() const noexcept { return width * height; }
  constexpr bool IsEmpty() const noexcept { return width <= 0 || height <= 0; }
  constexpr friend bool operator==(const Size&, const Size&) = default;
};

enum class VideoFormat {
  kUnknown = 0,
  kI420,
  kYV12,
  kNV12,
  kNV21,
  kYUY2,
  kARGB,
  kRGB24,
  kRGB565,
  kYUV420P10,
  kP010,
};

XMEDIA_MEDIA_EXPORT const char* GetVideoFormatName(VideoFormat format);
XMEDIA_MEDIA_EXPORT int VideoFormatPlaneCount(VideoFormat format);

// The strong-typed GPU handle carried by a non-CPU frame (xmedia-kit §6.2, the
// zero-copy contract). |kind| says what |id| points at, so a consumer never
// casts blind:
//
//   kVaapiSurface    id = VASurfaceID (as uintptr),  imported to EGLImage.
//   kD3D11Texture    id = ID3D11Texture2D*, subresource = SRV slice.
//   kCVPixelBuffer   id = CVPixelBufferRef,           importable to MTLTexture
//                    (or IOSurface-backed GL texture).
//   kNvdecFrame      id = CUdeviceptr of the NV12 luma plane; the chroma plane
//                    and strides travel in NvdecFrameInfo (see below).
//
// |id| stays owned by the frame's producer-side backing (typically an FFmpeg
// hw frame); it is only valid while this VideoFrame is alive. media/base
// never names those platform types directly (invariant C4) -- the kind tag is
// the contract, the platform layers do the casting.
enum class NativeHandleKind {
  kNone = 0,
  kVaapiSurface,
  kD3D11Texture,
  kCVPixelBuffer,
  kNvdecFrame,
};

XMEDIA_MEDIA_EXPORT const char* GetNativeHandleKindName(NativeHandleKind kind);

// One plane of a dmabuf-backed frame, in the vocabulary EGL's
// EGL_LINUX_DMA_BUF_EXT import attributes need: a prime fd, the bytes per row
// and the bytes from the start of the whole buffer. The fd is owned by the
// producer-side backing (the FFmpeg VAAPI adapter derived it from a VA
// surface) and is valid only while this VideoFrame is alive -- same lifetime
// rule as NativeHandle::id, see WrapNativeBuffer().
struct XMEDIA_MEDIA_EXPORT DmaBufPlane {
  int fd{-1};
  uint32_t stride{0};   // bytes per row of this plane.
  uint32_t offset{0};   // bytes from the start of the backing buffer.
};

// The per-frame dmabuf plumbing a Linux hardware frame carries when its
// producer derived the plane handles -- plain data on purpose, because the
// reader is the platform EGL importer (platform/hwimport), which must not see
// a libav or VA type (invariants C4/C5). |fourcc| is the DRM fourcc of the
// WHOLE buffer (NV12 etc.); the per-plane fourcc an EGLImage import needs
// (R8, GR88) is derived by the consumer from the frame's VideoFormat, which
// the decoder set as the same layout hint it always carries. A modifier of 0
// with |has_modifier| false means "linear / unknown", which is what
// vaAcquireBufferHandle reports; the importer then omits the modifier
// attributes instead of passing a wrong one.
struct XMEDIA_MEDIA_EXPORT DmaBufFrameInfo {
  uint32_t fourcc{0};
  int plane_count{0};
  bool has_modifier{false};
  uint64_t modifier{0};
  DmaBufPlane planes[4];
};

// The NVIDIA frame plumbing, in the same plain-data spirit as
// DmaBufFrameInfo: the reader is the platform CUDA-GL importer, which must not
// see a CUDA or libav type (invariants C4/C5). |cuda_ctx| is the CUcontext the
// decoder's hwdevice created -- the importer pushes it while it works, because
// the device pointers below belong to it. All pointers stay valid only while
// this VideoFrame is alive (the frame's release callback frees the NVDEC
// pool's memory), same lifetime rule as NativeHandle::id.
struct XMEDIA_MEDIA_EXPORT NvdecFrameInfo {
  const void* cuda_ctx{nullptr};
  uintptr_t luma{0};           // CUdeviceptr of the NV12 luma plane.
  uintptr_t chroma{0};         // CUdeviceptr of the NV12 chroma plane.
  int32_t luma_stride{0};      // bytes per row, the decoder's linesize.
  int32_t chroma_stride{0};
};

struct XMEDIA_MEDIA_EXPORT NativeHandle {
  NativeHandleKind kind{NativeHandleKind::kNone};
  const void* id{nullptr};
  int subresource{0};

  constexpr friend bool operator==(const NativeHandle&,
                                   const NativeHandle&) = default;
};

// A decoded video frame.
//
// All instances are reference-counted and must be held through
// scoped_refptr<VideoFrame>. Passing a frame to a sink therefore costs one
// atomic increment, never a pixel copy.
class XMEDIA_MEDIA_EXPORT VideoFrame
    : public base::RefCountedThreadSafe<VideoFrame> {
 public:
  REQUIRE_ADOPTION_FOR_REFCOUNTED_TYPE();

  enum class StorageType {
    kStorageOwned = 0,  // CPU memory allocated by xmedia-kit; planes readable.
    kStorageDmaBufs,    // Linux dmabuf; zero-copy into EGL, planes unreadable.
    kStorageGpuMemoryBuffer,
    kStorageOpaque,  // Platform-private handle (MediaCodec Surface,
                     // CVPixelBuffer). planes() returns an empty span.
  };

  enum Plane {
    kYPlane = 0,
    kUPlane = 1,
    kVPlane = 2,
    kAPlane = 3,
    kMaxPlanes = 4
  };

  VideoFormat format() const { return format_; }
  StorageType storage_type() const { return storage_type_; }
  Size coded_size() const { return coded_size_; }
  Size natural_size() const { return natural_size_; }
  Rational sar() const { return sar_; }
  int rotation() const { return rotation_; }

  // The size the picture covers once |rotation| has been applied: a quarter
  // turn lays the frame on its side, so the two axes swap, and 0 or 180 leaves
  // them alone. Layout code has to ask for THIS rather than `natural_size()`
  // -- fitting a rect against the undecorated size and then rotating squeezes
  // a landscape picture back into a landscape box, which is the one failure
  // here that looks like a rotation bug and is not.
  Size NaturalSizeForRotation() const;

  VideoColorSpace color_space() const { return color_space_; }

  base::TimeDelta timestamp() const { return timestamp_; }
  base::TimeDelta duration() const { return duration_; }
  // Wall-clock anchor meant to be captured when the frame is produced.
  // RESERVED, NOT WIRED: no producer sets it and no consumer reads it, so the
  // value is always a null TimeTicks. The compositor paces with the frame's
  // PTS, the master clock and the sink's [deadline_min, deadline_max] window
  // instead (media/filters/legacy/video_frame_compositor.cc,
  // BuildFrameSyncInputLocked); Chromium selects frames by this field. Kept
  // for API parity with Chromium -- do not build scheduling logic on it
  // without wiring the producers first. See docs/07 compositor checklist.
  base::TimeTicks reference_time() const { return reference_time_; }
  int32_t serial() const { return serial_; }
  int64_t frame_index() const { return frame_index_; }

  // CPU access. Returns an empty span for kStorageOpaque / kStorageDmaBufs.
  std::span<const uint8_t> visible_data(Plane plane) const;
  // Producer-side write access. Only the code that is filling a freshly
  // allocated frame (a decoder or a converter) may call this; once the frame is
  // handed to a sink it is treated as immutable, which is what lets the same
  // buffer be read concurrently by the compositor and the sink without a lock.
  std::span<uint8_t> mutable_data(Plane plane);
  int32_t stride(Plane plane) const { return strides_[plane]; }
  size_t allocated_size(Plane plane) const { return allocated_sizes_[plane]; }

  // Producer-side colour tagging, same window of validity as mutable_data():
  // the decoder sets it before the frame is first published.
  void set_color_space(VideoColorSpace cs) { color_space_ = cs; }

  // Producer-side display rotation, same window: the decoder copies it out of
  // the stream's VideoDecoderConfig, which is where the container's display
  // matrix ends up. A frame built by CreateBlackFrame() carries 0, so a path
  // that never sets this shows phone-shot material on its side -- the metadata
  // was parsed, reported to the host, and then dropped on the way to the
  // pixels.
  void set_rotation(int degrees) { rotation_ = degrees; }

  bool IsMappable() const {
    return storage_type_ == StorageType::kStorageOwned;
  }

  // Zero-copy contract (xmedia-kit §6.2): a hardware-decoded frame is passed
  // GPU
  // to GPU end to end; pixels are only read back when a consumer EXPLICITLY
  // calls ToI420(). Returns:
  //   - this same frame (new reference) when the pixels are already owned
  //     I420 CPU memory;
  //   - a freshly allocated owned I420 frame when the producer installed a
  //     readback path (the FFmpeg hw decoder does);
  //   - nullptr when no readback is possible (no producer path installed).
  // Never runs on the frame's own storage in place: the caller keeps using
  // the original frame for display while it reads.
  base::scoped_refptr<VideoFrame> ToI420() const;

  // Test/CLI helper: builds a zero-filled owned frame. Production decoders go
  // through VideoFramePool instead.
  static base::scoped_refptr<VideoFrame>
  CreateBlackFrame(VideoFormat format, Size coded_size, Size natural_size,
                   Rational sar, base::TimeDelta timestamp,
                   base::TimeDelta duration, int32_t serial);

  // The hardware-frame constructor. |handle| names the GPU object; it stays
  // owned by the producer's backing store and is only guaranteed valid while
  // this frame (and every reference cloned from it) is alive. |release_cb|
  // runs at last-unref and is what actually frees that backing store;
  // |to_i420_cb|, when set, is the explicit-readback path behind ToI420().
  // |format| is a LAYOUT hint (e.g. kNV12), not a CPU-plane promise --
  // planes() stays empty for this storage.
  static base::scoped_refptr<VideoFrame> WrapNativeBuffer(
      NativeHandle handle, VideoFormat format, Size coded_size,
      Size natural_size, Rational sar, base::TimeDelta timestamp,
      base::TimeDelta duration, int32_t serial, base::OnceClosure release_cb,
      base::RepeatingCallback<base::scoped_refptr<VideoFrame>()> to_i420_cb);

  // The typed GPU handle. Empty (kNone) unless the producer used
  // WrapNativeBuffer(). Supersedes opaque_handle(), which stays for the
  // dmabuf adapters that predate the kind tag.
  NativeHandle native_handle() const { return native_handle_; }

  // The dmabuf plumbing a Linux hardware frame carries when its producer
  // derived per-plane prime fds from the native handle (the FFmpeg VAAPI
  // adapter). Null when the producer did not derive them -- a host without
  // derivation falls back to the explicit readback below, which is the
  // documented degradation. The pointee is owned by this frame.
  const DmaBufFrameInfo* dmabuf_info() const { return dmabuf_info_.get(); }

  // Producer-side attach, same validity window as mutable_data(): the decoder
  // calls it before the frame is first published, and callers treat the frame
  // as immutable afterwards. Attaching also re-tags storage from kStorageOpaque
  // to kStorageDmaBufs, which is what the tag means. Passing null is a no-op
  // (a producer that derived nothing simply does not call this).
  void set_dmabuf_info(std::unique_ptr<DmaBufFrameInfo> info);

  // The NVIDIA plumbing a hardware frame carries when the producer decoded
  // with NVDEC (see NvdecFrameInfo). Null when absent; the pointee is owned by
  // this frame. Attaching does NOT change the storage tag: the tag stays
  // kStorageOpaque because the import route (CUDA-GL interop) is a different
  // one from the dmabuf route and the GL layer only needs to know the planes
  // are unreadable -- which kStorageOpaque already means.
  const NvdecFrameInfo* nvdec_info() const { return nvdec_info_.get(); }

  // Producer-side attach, same validity window as set_dmabuf_info().
  void set_nvdec_info(std::unique_ptr<NvdecFrameInfo> info);

  // Adapter escape hatch for platform code (FFmpeg / dmabuf). See
  // DecoderBuffer::storage_as() for the rationale; no RTTI involved.
  const void* opaque_handle() const { return opaque_handle_; }

  std::string AsDebugString() const;

 private:
  friend class base::RefCountedThreadSafe<VideoFrame>;
  friend class VideoFramePool;
  ~VideoFrame();

  VideoFrame();

  VideoFormat format_{VideoFormat::kUnknown};
  StorageType storage_type_{StorageType::kStorageOwned};
  Size coded_size_;
  Size natural_size_;
  Rational sar_{1, 1};
  int rotation_{0};
  base::TimeDelta timestamp_;
  base::TimeDelta duration_;
  base::TimeTicks
      reference_time_;  // See reference_time(): reserved, never written.
  int32_t serial_{0};
  int64_t frame_index_{-1};
  std::array<uint8_t*, kMaxPlanes> planes_{nullptr, nullptr, nullptr, nullptr};
  std::array<int32_t, kMaxPlanes> strides_{0, 0, 0, 0};
  std::array<size_t, kMaxPlanes> allocated_sizes_{0, 0, 0, 0};
  const void* opaque_handle_{nullptr};
  // Dmabuf plumbing attached by the producer (see dmabuf_info()). Unique to
  // this frame: two references cloned from one wrap share the native handle
  // by value, but the derived fds are released with THIS frame's release
  // callback, so the info object must not be shared with another frame.
  std::unique_ptr<DmaBufFrameInfo> dmabuf_info_;
  // NVIDIA frame plumbing attached by the producer (see nvdec_info()).
  std::unique_ptr<NvdecFrameInfo> nvdec_info_;
  void* allocation_{nullptr};  // Owned backing store when storage is owned.
  NativeHandle native_handle_;
  VideoColorSpace color_space_;
  // Non-owned storage (hw frames, dmabufs): the producer-side teardown. Run
  // exactly once, at last unref, before the frame's memory goes away.
  base::OnceClosure release_cb_;
  // The explicit-readback path behind ToI420(); installed by the producer
  // that created a native-buffer frame. Null = no readback possible.
  base::RepeatingCallback<base::scoped_refptr<VideoFrame>()> to_i420_cb_;
};

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_BASE_VIDEO_FRAME_H_

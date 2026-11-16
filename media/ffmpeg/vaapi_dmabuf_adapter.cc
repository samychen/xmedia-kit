// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/vaapi_dmabuf_adapter.h"

// The whole body is opt-in: this file sits in the xmedia_ffmpeg source glob
// and is compiled by every FFmpeg build, including hosts without libva
// (macOS, no-vaapi Linux distros). XMEDIA_ENABLE_VAAPI is the build flag the
// CMake layer defines exactly when libva was found; without it the
// translation unit is empty and the decoder's own #if keeps the derivation
// call out as well.
#if defined(XMEDIA_ENABLE_VAAPI)

#include <atomic>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"

namespace xmedia::media::ffmpeg {
namespace {

// The DRM fourcc of the whole buffer, for the DmaBufFrameInfo tag. VA fourccs
// are the same four-character codes DRM uses for the bi-planar layouts
// (VA_FOURCC_NV12 and DRM_FORMAT_NV12 are the same uint32), so the derived
// image's own fourcc passes through as-is; the per-plane fourcc an EGLImage
// import needs is the consumer's business (egl_dmabuf_importer.cc).
//
// The 10-bit bi-planar layouts are declined rather than passed through:
// reporting a P010 surface to the display layer would make a consumer that
// ignores VideoFormat sample 16-bit elements as if they were 8-bit -- the
// format gate belongs to the producer, not to a call site per consumer.
bool FormatSupported(VideoFormat format) {
  return format == VideoFormat::kNV12 || format == VideoFormat::kI420;
}

}  // namespace

VaapiDmabufDerive DeriveVaapiDmabufs(VADisplay display, uintptr_t surface_id,
                                     int width, int height,
                                     VideoFormat format) {
  VaapiDmabufDerive result;
  if (display == nullptr || !FormatSupported(format)) {
    return result;
  }

  // vaDeriveImage creates a VAImage that REFERENCES the surface's storage
  // (no pixel copy); its buffer is what carries the prime fd.
  VAImage image{};
  VAStatus status = vaDeriveImage(display,
                                  static_cast<VASurfaceID>(surface_id),
                                  &image);
  if (status != VA_STATUS_SUCCESS) {
    // The first refusal is the interesting one; a driver that refuses this
    // layout refuses it for every frame of the stream.
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
      LOG(WARNING) << "vaDeriveImage declined the surface layout (status "
                   << static_cast<int>(status)
                   << "); VAAPI frames will stay readback-only";
    }
    return result;
  }

  // The image's own buffer is the object with an external handle. The fd is
  // acquired for the frame's lifetime and released by |cleanup| below.
  VABufferInfo buffer_info{};
  buffer_info.mem_type = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME;
  status = vaAcquireBufferHandle(display, image.buf, &buffer_info);
  if (status != VA_STATUS_SUCCESS) {
    LOG(WARNING) << "vaAcquireBufferHandle declined (status "
                 << static_cast<int>(status)
                 << "); VAAPI frames will stay readback-only";
    vaDestroyImage(display, image.image_id);
    return result;
  }

  // VAImageFormat.fourcc is the layout tag of the WHOLE buffer; the plane
  // table is the image's own pitches/offsets, which is exactly what the
  // EGL import attributes need per plane. NV12 and I420 derived images are
  // one fd with per-plane offsets -- the same fd appears in every plane row.
  auto info = std::make_unique<DmaBufFrameInfo>();
  info->fourcc = image.format.fourcc;
  info->has_modifier = false;  // vaAcquireBufferHandle reports no modifier.
  info->modifier = 0;
  const int fd = static_cast<int>(buffer_info.handle);
  const int plane_count = image.num_planes;
  for (int i = 0; i < plane_count && i < 4; ++i) {
    info->planes[i].fd = fd;
    info->planes[i].stride = image.pitches[i];
    info->planes[i].offset = image.offsets[i];
  }
  info->plane_count = plane_count;

  // Cleanup owns the two resources the derivation created, in release order:
  // the buffer handle first (it pins the fd), then the image. The VADisplay
  // is passed through as a raw pointer, which is safe because the caller
  // merges this closure into the frame's release callback BEFORE freeing the
  // AVFrame the frame holds -- that AVFrame references the hardware frames
  // context, which references the device context the display belongs to, so
  // the display is still alive for as long as the closure runs.
  result.info = std::move(info);
  result.cleanup = base::BindOnce(
      [](VADisplay display, VABufferID buffer_id, VAImageID image_id) {
        vaReleaseBufferHandle(display, buffer_id);
        vaDestroyImage(display, image_id);
      },
      display, image.buf, image.image_id);
  return result;
}

#endif  // XMEDIA_ENABLE_VAAPI

}  // namespace xmedia::media::ffmpeg

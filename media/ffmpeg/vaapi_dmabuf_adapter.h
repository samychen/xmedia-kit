// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FFMPEG_VAAPI_DMABUF_ADAPTER_H_
#define XMEDIA_MEDIA_FFMPEG_VAAPI_DMABUF_ADAPTER_H_

// The VA half of the Linux zero-copy path: a VASurfaceID by itself is not
// something EGL can name -- EGL imports dmabuf fds, so the producer has to
// walk the surface through vaDeriveImage() + vaAcquireBufferHandle() once per
// frame and hand out the per-plane {fd, stride, offset} triples. This is the
// same route mpv's vaapi-egl and Chromium's VaapiDrmPicture take; nothing
// here reads pixels.
//
// WHY PER FRAME. The derived VAImage pins the surface's storage, and a VA
// pool recycles surfaces aggressively; caching handles keyed on the surface
// ID would hand frame N's buffer to frame N+1. Deriving per frame is four
// cheap VA calls and no pixel copy, and releasing through the frame's own
// release callback makes the lifetime rule trivial: the fd dies with the
// frame that published it.
//
// This file lives in media/ffmpeg and not platform/ because the VA types are
// vendor types (invariant C5 exempts this directory for the same reason it
// exempts the libav includes) and because the caller -- the hw decoder that
// created the VASurfaceID and owns its VADisplay -- already lives here.

#include <memory>

#include "base/functional/callback.h"
#include "media/base/video_frame.h"

// va.h is a vendor header, allowed in media/ffmpeg/ like the libav ones;
// a forward declaration is not enough because the caller already HAS the
// display object and only passes the pointer through. va_drmcommon.h carries
// VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME, the mem type the buffer-handle
// acquisition asks for.
//
// The result struct is defined unconditionally: the hw decoder declares a
// local of it outside its own #if and attaches it to the wrapped frame, so
// non-VA builds need the type to exist (with both members always null --
// the attach points then degrade to no-ops). Only the VA-touching parts are
// guarded.
#if defined(XMEDIA_ENABLE_VAAPI)
#include <va/va.h>
#include <va/va_drmcommon.h>
#endif

namespace xmedia::media::ffmpeg {

// What one successful derivation hands back. |info| is null when the VA
// driver declined the derivation -- some drivers refuse vaDeriveImage for
// their internal surface layouts. A null result is NOT an error: the display
// side answers it with the documented fallback (one explicit readback per
// frame), which is the same behavior this build had before the derivation
// existed.
struct VaapiDmabufDerive {
  std::unique_ptr<DmaBufFrameInfo> info;
  // Runs exactly once when the owning frame dies: releases the acquired
  // buffer handle and destroys the derived image, in that order, while the
  // surface reference the frame holds keeps the storage alive.
  base::OnceClosure cleanup;
};

#if defined(XMEDIA_ENABLE_VAAPI)
// Derives the dmabuf plane handles of |surface_id| on |display|.
// |format| is the same layout hint the decoder puts on the VideoFrame
// (kNV12 today; 10-bit layouts are declined here and stay readback-only,
// because the display shader reads 8-bit planes). Never throws; a failure is
// an empty result.
VaapiDmabufDerive DeriveVaapiDmabufs(VADisplay display, uintptr_t surface_id,
                                     int width, int height,
                                     VideoFormat format);
#endif  // XMEDIA_ENABLE_VAAPI

}  // namespace xmedia::media::ffmpeg

#endif  // XMEDIA_MEDIA_FFMPEG_VAAPI_DMABUF_ADAPTER_H_

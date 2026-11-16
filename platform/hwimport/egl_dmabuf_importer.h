// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_PLATFORM_HWIMPORT_EGL_DMABUF_IMPORTER_H_
#define XMEDIA_PLATFORM_HWIMPORT_EGL_DMABUF_IMPORTER_H_

#include <memory>

#include "media/gl/gl_hw_texture_importer.h"

namespace xmedia::platform::hwimport {

// The display half of the Linux zero-copy path: EGLImage import over the
// per-plane prime fds the producer derived (media/ffmpeg/vaapi_dmabuf_adapter
// on the decode side; a test or a future GBM/v4l2 producer on any other).
//
// HOW ONE PLANE GOES IN. eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT) with the
// plane's fd/stride/offset and its own fourcc -- R8 for a luma plane, GR88
// for NV12's interleaved chroma -- then glEGLImageTargetTexture2DOES points
// the CALLER'S texture at the image's storage. The image handle is destroyed
// right after the texture-target call: the texture holds the storage
// reference, and keeping the handle would leak one EGLImage per frame per
// plane. One NV12 frame is therefore two EGLImages over the same fd at
// different offsets, which is exactly the layout VaapiDrmPicture in Chromium
// hands the GPU.
//
// WHICH DISPLAY. None is passed in: ImportPlane() runs on the thread that has
// the sink's context current (see the interface's threading rule), and
// eglGetCurrentDisplay() IS the sink's display there. An EGLImage created on
// display X is only usable by contexts on display X, so taking the current
// one is not a shortcut, it is the requirement. A GLX host (GLFW's default
// backend on X11) has no current EGL display, and the import fails on the
// first plane -- one log line, then the layer's sticky fallback reads the
// frame back, which is the documented degradation for a host whose graphics
// stack cannot do the import. Zero-copy on Linux is delivered through the
// EGL sink or an EGL-context host; the importer never pretends otherwise.
//
// ONLY 8-BIT 4:2:0. CanImport accepts the NV12 and I420 layouts the shader
// reads as R8 (+GR88 for the interleaved chroma); a P010 surface is declined
// by the PRODUCER side and would be declined here too. A 10-bit import would
// need a different shader and a different matrix range, and sampling 16-bit
// elements as if they were 8-bit is the failure mode the format gates exist
// to prevent.
std::unique_ptr<media::GlHwTextureImporter> CreateEglDmabufImporter();

}  // namespace xmedia::platform::hwimport

#endif  // XMEDIA_PLATFORM_HWIMPORT_EGL_DMABUF_IMPORTER_H_

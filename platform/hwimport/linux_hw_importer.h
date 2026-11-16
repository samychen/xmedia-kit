// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_PLATFORM_HWIMPORT_LINUX_HW_IMPORTER_H_
#define XMEDIA_PLATFORM_HWIMPORT_LINUX_HW_IMPORTER_H_

#include <memory>

#include "media/gl/gl_hw_texture_importer.h"

namespace xmedia::platform::hwimport {

// The Linux display-side import chain: VAAPI dmabuf -> EGLImage, NVDEC ->
// CUDA-GL interop, routed per frame by CanImport. Built only when the CMake
// layer found the SDKs (EGL and/or CUDA); see platform/CMakeLists.txt.
std::unique_ptr<media::GlHwTextureImporter> CreateLinuxHwImporter();

}  // namespace xmedia::platform::hwimport

#endif  // XMEDIA_PLATFORM_HWIMPORT_LINUX_HW_IMPORTER_H_

// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_RENDER_GL_GL_EXPORT_H_
#define XMEDIA_RENDER_GL_GL_EXPORT_H_

// Symbol visibility for the xmedia_gl module.
//
// The whole project builds with `CMAKE_CXX_VISIBILITY_PRESET hidden`, so a
// class a consumer outside this module must be able to name, derive from or
// call needs the default visibility explicitly. Only the module's own sources
// define XMEDIA_GL_IMPLEMENTATION (see render/CMakeLists.txt), which keeps the
// annotation meaningless-but-harmless everywhere else.
#if defined(XMEDIA_GL_IMPLEMENTATION)
#define XMEDIA_GL_EXPORT __attribute__((visibility("default")))
#else
#define XMEDIA_GL_EXPORT
#endif

#endif  // XMEDIA_RENDER_GL_GL_EXPORT_H_

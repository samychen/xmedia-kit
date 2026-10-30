// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_PLATFORM_EGL_EGL_GL_CONTEXT_H_
#define XMEDIA_PLATFORM_EGL_EGL_GL_CONTEXT_H_

#include "render/gl/native_gl_context.h"

namespace xmedia::media {

// NativeGLContext backed by EGL. Wraps host-owned EGLDisplay / EGLSurface /
// EGLContext -- the common path for embedded, surfaceless and mobile GL.
// Borrowed semantics -- the destructor does NOT destroy the host's objects.
//
// NOTE on GetProcAddress: eglGetProcAddress resolves GL core and extension
// entry points on most EGL implementations; where a platform exposes the
// core GL functions only through its loader (or dlsym), extend this method
// accordingly. The SDL2 and GLFW backends avoid the question by using the
// window-system's own resolver.
class EglGLContext final : public gl::NativeGLContext {
 public:
  EglGLContext(void* display, void* surface, void* context);
  ~EglGLContext() override = default;

  void* GetProcAddress(const char* name) override;
  bool MakeCurrent() override;
  void ReleaseCurrent() override;
  void SwapBuffers() override;
  void GetDrawableSize(int* width, int* height) override;
  std::string GetError() override;

 private:
  void* display_;
  void* surface_;
  void* context_;
};

}  // namespace xmedia::media

#endif  // XMEDIA_PLATFORM_EGL_EGL_GL_CONTEXT_H_

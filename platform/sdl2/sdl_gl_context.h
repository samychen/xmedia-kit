// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_PLATFORM_SDL2_SDL_GL_CONTEXT_H_
#define XMEDIA_PLATFORM_SDL2_SDL_GL_CONTEXT_H_

#include "render/gl/native_gl_context.h"

namespace xmedia::media {

// NativeGLContext backed by SDL2: wraps a host-owned SDL_Window* and
// SDL_GLContext. Borrowed semantics -- the destructor does NOT destroy the
// host's context (per the embedding rules in docs/archive/09).
class SdlGLContext final : public gl::NativeGLContext {
 public:
  SdlGLContext(void* window, void* context);
  ~SdlGLContext() override = default;

  void* GetProcAddress(const char* name) override;
  bool MakeCurrent() override;
  void ReleaseCurrent() override;
  void SwapBuffers() override;
  void GetDrawableSize(int* width, int* height) override;
  std::string GetError() override;

 private:
  void* window_;
  void* context_;
};

}  // namespace xmedia::media

#endif  // XMEDIA_PLATFORM_SDL2_SDL_GL_CONTEXT_H_

// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_PLAYER_PUBLIC_PLAYER_EXPORT_H_
#define XMEDIA_PLAYER_PUBLIC_PLAYER_EXPORT_H_

#if defined(XMEDIA_PLAYER_IMPLEMENTATION)
#define XMEDIA_PLAYER_EXPORT __attribute__((visibility("default")))
#else
#define XMEDIA_PLAYER_EXPORT
#endif

#endif  // XMEDIA_PLAYER_PUBLIC_PLAYER_EXPORT_H_

// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_CURL_MEDIA_CURL_EXPORT_H_
#define XMEDIA_MEDIA_CURL_MEDIA_CURL_EXPORT_H_

#if defined(XMEDIA_CURL_IMPLEMENTATION)
#define XMEDIA_CURL_EXPORT __attribute__((visibility("default")))
#else
#define XMEDIA_CURL_EXPORT
#endif

#endif  // XMEDIA_MEDIA_CURL_MEDIA_CURL_EXPORT_H_

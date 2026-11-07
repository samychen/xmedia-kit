// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_CEF_CFC_RECTS_SCRIPT_H_
#define XMEDIA_MEDIA_CFC_RECTS_SCRIPT_H_

#include <string>

#include "media/media_export.h"

namespace xmedia::media::cefc {

// The rects-collection JS (scans [data-start],[data-duration],[data-z],
// measures getBoundingClientRect at dsf=1, reports mc.html.rects:<json> via
// cefQuery; self-reports t=0 at inject). Hosts inject it ONCE after first
// paint via ExecuteJavaScript; bake seek re-injects per tick. Ported from
// the composition SDK's MakeRectsScript (html_layer_producer.cpp).
std::string MakeRectsScript();

}  // namespace xmedia::media::cefc

#endif  // XMEDIA_MEDIA_CFC_RECTS_SCRIPT_H_

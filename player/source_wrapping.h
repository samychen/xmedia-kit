// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_PLAYER_SOURCE_WRAPPING_H_
#define XMEDIA_PLAYER_SOURCE_WRAPPING_H_

#include "base/memory/scoped_refptr.h"
#include "media/base/data_source_descriptor.h"
#include "media/filters/retry_data_source.h"
#include "player/public/player_config.h"

namespace xmedia {
namespace player_internal {

// Production source wiring, split out of player_impl_prepare.cc for the C1
// line limit (the seam is "which DataSource actually serves this URI").
// http(s) URIs leave FFmpeg's protocol layer and enter the DataSource bridge
// as an avio-backed UrlDataSource -- or, when the build has libcurl and
// NetConfig::prefer_curl asks for it, the CicadaPlayer-parity CurlDataSource
// -- wrapped in RetryDataSource. The protocol layer's own
// reconnect only covers mid-stream errors; a reset during connect or a hard
// reset mid-transfer surfaces as a read error HERE, and the retry decorator
// is what turns it into a fresh connection. rtmp/rtsp/srt stay on the
// protocol layer (streaming semantics the bridge does not model).
// |out_retry| is filled only when |track| is set -- the main source keeps
// its decorator named for diagnostics / ReconnectNow, while secondary
// sources keep theirs alive inside the wrapped descriptor's |custom| and
// need no separate handle.
media::DataSourceDescriptor
WrapRetry(const media::DataSourceDescriptor& descriptor, const NetConfig& net,
          bool track, base::scoped_refptr<media::RetryDataSource>* out_retry);

// Play-through caching, also split out for C1: when
// DataSourceConfig::enable_cache is on, every descriptor that carries a URI
// gets a FileCacheDataSource around its (already retry-wrapped) source --
// bytes the demuxer pulls are appended to a local file, and a later session
// with the same URI serves that prefix from disk without touching the
// network. Descriptors without a URI (memory buffers, fds) have nothing to
// key a cache file on and pass through untouched.
media::DataSourceDescriptor WrapCache(
    const media::DataSourceDescriptor& descriptor,
    const DataSourceConfig& config);

}  // namespace player_internal
}  // namespace xmedia

#endif  // XMEDIA_PLAYER_SOURCE_WRAPPING_H_

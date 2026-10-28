// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "player/source_wrapping.h"

#include <utility>

#include "avbase/BuildConfig.h"
#include "base/functional/bind.h"
#include "media/curl/curl_data_source.h"
#if AVBASE_ENABLE_FFMPEG
#include "media/ffmpeg/url_data_source.h"
#endif
#include "media/filters/file_cache_data_source.h"
#include "media/filters/retry_data_source.h"

namespace avbase {
namespace player_internal {

media::DataSourceDescriptor
WrapRetry(const media::DataSourceDescriptor& descriptor, const NetConfig& net,
          bool track, base::scoped_refptr<media::RetryDataSource>* out_retry) {
#if AVBASE_ENABLE_CURL
  // The curl transport is the CicadaPlayer-parity path: pinned DNS, forced
  // IPv4/IPv6, a proxy, and the low-speed watchdog all come from
  // CurlDataSource::Config, none of which the avio layer can express. It is
  // strictly opt-in (NetConfig::prefer_curl), so the avio path below stays
  // the default and the corpus gates keep comparing like with like. The
  // Retry decorator wraps it exactly like UrlDataSource: a connection reset
  // mid-transfer surfaces as a read error here either way.
  if (descriptor.kind == media::DataSourceDescriptor::Kind::kUri &&
      net.reconnect && net.prefer_curl &&
      media::curl::CurlDataSource::SupportsUri(descriptor.uri)) {
    media::curl::CurlDataSource::Config curl_config;
    curl_config.user_agent = net.user_agent;
    curl_config.headers = net.headers;
    auto curl_source = base::MakeRefCounted<media::curl::CurlDataSource>(
        descriptor.uri, std::move(curl_config));
    auto retry = base::MakeRefCounted<media::RetryDataSource>(
        std::move(curl_source),
        media::RetryDataSource::Config{net.reconnect_max_retries,
                                       net.reconnect_delay});
    if (track && out_retry) {
      *out_retry = retry;
    }
    auto wrapped = media::DataSourceDescriptor::FromSource(std::move(retry));
    wrapped.uri = descriptor.uri;
    return wrapped;
  }
#endif
#if AVBASE_ENABLE_FFMPEG
  if (descriptor.kind == media::DataSourceDescriptor::Kind::kUri &&
      (descriptor.uri.starts_with("http://") ||
       descriptor.uri.starts_with("https://")) &&
      net.reconnect) {
    auto url_source =
        base::MakeRefCounted<media::ffmpeg::UrlDataSource>(descriptor.uri);
    auto retry = base::MakeRefCounted<media::RetryDataSource>(
        std::move(url_source),
        media::RetryDataSource::Config{net.reconnect_max_retries,
                                       net.reconnect_delay});
    // Named BEFORE the descriptor takes ownership, never after: moving
    // |retry| into FromSource() below leaves it null, and a ReconnectNow()
    // that holds an empty handle fails silently -- which is what an ordering
    // this subtle deserves a warning about. (Found by clang-tidy's
    // bugprone-use-after-move; no test had noticed.)
    if (track && out_retry) {
      *out_retry = retry;
    }
    // Keep the URI for logs and MediaInfo: the hop below replaces the
    // descriptor with a URI-less bridge, and source_.uri is guarded by
    // state_lock_, so the copy arrives from |descriptor| instead.
    auto wrapped = media::DataSourceDescriptor::FromSource(std::move(retry));
    wrapped.uri = descriptor.uri;
    return wrapped;
  }
  return descriptor;
#else
  (void)net;
  (void)track;
  (void)out_retry;
  return descriptor;
#endif
}

media::DataSourceDescriptor
WrapCache(const media::DataSourceDescriptor& descriptor,
          const DataSourceConfig& config) {
  // Only URIs can key a cache file; a memory buffer or fd has nothing stable
  // to hash, and only descriptors that already carry a custom source (the
  // retry-wrapped URI sources) have something to wrap -- a bare kUri still
  // belongs to the protocol layer.
  if (!config.enable_cache || config.cache_dir.empty() ||
      descriptor.uri.empty() ||
      (descriptor.kind != media::DataSourceDescriptor::Kind::kUri &&
       descriptor.kind !=
           media::DataSourceDescriptor::Kind::kCustomSource) ||
      descriptor.custom == nullptr) {
    return descriptor;
  }
  auto cached = base::MakeRefCounted<media::FileCacheDataSource>(
      descriptor.custom,
      media::FileCacheDataSource::Config{descriptor.uri, config.cache_dir,
                                         config.cache_max_bytes});
  auto wrapped = media::DataSourceDescriptor::FromSource(std::move(cached));
  wrapped.uri = descriptor.uri;
  return wrapped;
}

}  // namespace player_internal
}  // namespace avbase

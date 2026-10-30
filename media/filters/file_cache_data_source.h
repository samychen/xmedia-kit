// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FILTERS_FILE_CACHE_DATA_SOURCE_H_
#define XMEDIA_MEDIA_FILTERS_FILE_CACHE_DATA_SOURCE_H_

#include <stdint.h>

#include <cstddef>
#include <mutex>
#include <string>

#include "base/task/task_runner.h"
#include "base/time/time.h"
#include "media/base/data_source.h"
#include "media/base/media_error.h"
#include "media/filters/async_data_source_base.h"
#include "media/media_export.h"

namespace xmedia::media {

// A DataSource decorator that plays through a growing local file -- the
// byte-level port of CicadaPlayer's cacheModule ("边播边存"). Every byte the
// demuxer pulls is appended to a cache file keyed by the source URI; a later
// session whose cache file is complete-enough serves those bytes from disk
// and never touches the network for them (restart-resume, the property the
// ijkio framework existed to provide and that M18's config fields
// data_source.enable_cache / cache_dir / cache_max_bytes have been reserved
// for since they appeared in PlayerConfig).
//
// CACHE DISCIPLINE.
//   * Open: the cache file (|cache_dir|/<fnv1a(url)>.avcache) is opened for
//     read+write. Its size is the authoritative "already downloaded" mark.
//   * A read fully inside the mark is served from disk; the inner source is
//     not opened at all -- that is the resume path.
//   * A read at/after the mark reads through from the inner source and
//     appends what it got, advancing the mark. Reads strictly above the
//     mark but below it after the append are served on the next pass, so
//     the file grows as a prefix -- no holes, no re-download.
//   * Appends stop at |max_bytes| (read-through only beyond it), and ANY
//     file error falls back to pure read-through: a broken cache directory
//     must degrade the session to "no cache", never fail playback.
//
// THREADING. ReadBlocking runs on the demux thread (D3) under a lock; the
// async Read() runs on the shared AsyncDataSourceBase worker. Abort() stops
// both and forwards to the inner source (Delta 15).
class XMEDIA_MEDIA_EXPORT FileCacheDataSource final : public DataSource {
 public:
  struct Config {
    // Key for the cache file name; the URI the descriptor carried.
    std::string url;
    std::string cache_dir;
    size_t max_bytes{512ULL * 1024 * 1024};
  };

  FileCacheDataSource(base::scoped_refptr<DataSource> inner, Config config);
  FileCacheDataSource(const FileCacheDataSource&) = delete;
  FileCacheDataSource& operator=(const FileCacheDataSource&) = delete;
  ~FileCacheDataSource() override;

  // Observability for tests: bytes persisted so far, and whether the cache
  // file was usable at all (false when the directory is unwritable).
  int64_t cached_bytes() const;
  bool cache_active() const;

  // ---- DataSource ----------------------------------------------------------
  void SetHost(Host* host) override;
  void Read(int64_t offset, size_t size, uint8_t* data,
            base::scoped_refptr<base::TaskRunner> task_runner,
            ReadCB read_cb) override;
  ReadResult ReadBlocking(int64_t offset, size_t size, uint8_t* data) override;
  void Abort() override;
  bool GetSize(int64_t* size_out) override;
  bool IsStreaming() const override;
  void SetBitrate(int bitrate) override;
  bool IsSeekable() const override;

 private:
  // Opens/creates the cache file and adopts its size as the download mark.
  // Failures leave cache_active() false and the source in read-through.
  void OpenCacheFileLocked();
  // Serves [offset, offset+size) from the cache file where it is inside the
  // mark. Returns bytes served (may be 0).
  size_t ReadFromCacheLocked(int64_t offset, size_t size, uint8_t* data);
  // Appends inner bytes to the cache file, growing the mark. Best effort.
  void AppendToCacheLocked(const uint8_t* data, size_t bytes);

  base::scoped_refptr<DataSource> inner_;
  const Config config_;

  mutable std::mutex lock_;
  void* cache_file_ = nullptr;  // std::FILE*, void* to keep <cstdio> out of
                                // the header surface.
  int64_t downloaded_mark_ = 0;
  bool cache_active_ = false;

  // Shared async-Read plumbing. Declared last, like every decorator here.
  AsyncDataSourceBase async_;
};

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_FILTERS_FILE_CACHE_DATA_SOURCE_H_

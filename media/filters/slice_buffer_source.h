// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FILTERS_SLICE_BUFFER_SOURCE_H_
#define XMEDIA_MEDIA_FILTERS_SLICE_BUFFER_SOURCE_H_

#include <stdint.h>

#include <cstddef>
#include <list>
#include <map>
#include <mutex>
#include <vector>

#include "base/task/task_runner.h"
#include "media/base/data_source.h"
#include "media/base/media_error.h"
#include "media/filters/async_data_source_base.h"
#include "media/media_export.h"

namespace xmedia::media {

// A DataSource decorator that caches the byte ranges it has served in
// fixed-size memory slices with a total byte budget -- the port of
// CicadaPlayer's data_source/cache slice cache (memPoolSlice + sliceBuffer-
// Source). Its consumer is any reader that revisits offsets: the FFmpeg
// probe re-reads the head, mp4 boxes are read twice (size then payload),
// and a seek backwards replays bytes the source already paid for. A miss
// reads the inner source one whole slice (so the neighbouring bytes are
// cached for the sequential reader), a hit never touches it.
//
// EVICTION. When the budget is exceeded the least-recently-used slice is
// dropped (an intrusive LRU list of slice indices). Slices are whole units:
// nothing is partially evicted, so a cached range never has holes.
//
// THREADING. ReadBlocking runs on the demux thread (D3) under a lock; the
// async Read() runs on the shared AsyncDataSourceBase worker. Abort() stops
// both and forwards to the inner source, so Stop() stays bounded (Delta 15).
class XMEDIA_MEDIA_EXPORT SliceBufferSource final : public DataSource {
 public:
  struct Config {
    size_t slice_size{64 * 1024};
    size_t max_bytes{64 * 1024 * 1024};
  };

  SliceBufferSource(base::scoped_refptr<DataSource> inner, Config config);
  SliceBufferSource(const SliceBufferSource&) = delete;
  SliceBufferSource& operator=(const SliceBufferSource&) = delete;
  ~SliceBufferSource() override;

  // Bytes currently held in slices; tests use it to observe eviction.
  size_t cached_bytes() const;

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
  // Copies what the slices hold for [offset, offset+size), returning the
  // number of leading bytes served (0 when the range starts on a miss).
  size_t CopyFromCacheLocked(int64_t offset, size_t size, uint8_t* data);
  // Reads one whole slice from the inner source and stores it. Returns the
  // slice bytes actually read (short only at EOF), or the inner error.
  ReadResult LoadSliceLocked(int64_t slice_start);

  base::scoped_refptr<DataSource> inner_;
  const Config config_;

  mutable std::mutex lock_;
  std::map<int64_t, std::vector<uint8_t>> slices_;  // slice start -> bytes
  std::list<int64_t> lru_;                          // front = most recent
  size_t cached_bytes_ = 0;

  // Shared async-Read plumbing. Declared last, like every decorator here,
  // so the state above is ready before its worker runs ReadBlocking.
  AsyncDataSourceBase async_;
};

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_FILTERS_SLICE_BUFFER_SOURCE_H_

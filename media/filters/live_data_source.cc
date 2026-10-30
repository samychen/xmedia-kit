// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/live_data_source.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#include "base/functional/bind.h"

namespace xmedia::media {
namespace {

// Abort() must bound every wait (Δ15); the edge wait polls in slices of
// this size so an abort or an append is noticed promptly.
constexpr std::chrono::milliseconds kEdgeWaitSlice(10);

}  // namespace

LiveDataSource::LiveDataSource()
    : data_cv_(&lock_),
      async_("xmedia-live-src",
             base::BindRepeating(&LiveDataSource::ReadBlocking,
                                 base::Unretained(this))) {}

LiveDataSource::~LiveDataSource() {
  Close();
  Abort();
  async_.StopWorker();
}

void LiveDataSource::Append(const uint8_t* data, size_t size) {
  if (size == 0) {
    return;
  }
  {
    base::AutoLock scoped(lock_);
    buffer_.insert(buffer_.end(), data, data + size);
    data_cv_.Broadcast();
  }
}

size_t LiveDataSource::appended_size() const {
  base::AutoLock scoped(lock_);
  return buffer_.size();
}

void LiveDataSource::Close() {
  {
    base::AutoLock scoped(lock_);
    closed_ = true;
    data_cv_.Broadcast();
  }
}

void LiveDataSource::SetHost(Host* /*host*/) {
  // A live byte source has no demuxer-facing host needs; the bridge owns
  // the demuxer side. Kept for the contract.
}

DataSource::ReadResult LiveDataSource::ReadBlocking(int64_t offset, size_t size,
                                                    uint8_t* data) {
  if (offset < 0) {
    return Err(ErrorCode::kInvalidArgument, "negative read offset",
               "offset = " + std::to_string(offset),
               "a streaming source reads sequentially from 0");
  }
  while (true) {
    {
      base::AutoLock scoped(lock_);
      const size_t available =
          buffer_.size() > static_cast<size_t>(offset)
              ? buffer_.size() - static_cast<size_t>(offset)
              : 0;
      if (available > 0) {
        // Partial serves are the live contract: deliver what exists now.
        const size_t n = std::min(available, size);
        std::memcpy(data, buffer_.data() + offset, n);
        return static_cast<int>(n);
      }
      if (closed_) {
        return 0;  // EOF: the stream was finite after all.
      }
      if (async_.IsAborted()) {
        return Err(ErrorCode::kAborted, "the live source was aborted",
                   "offset = " + std::to_string(offset),
                   "this follows Stop(); no action needed");
      }
    }
    // At the live edge: the producer has nothing new yet. Wait in slices so
    // Abort()/Close()/Append() are all noticed promptly.
    std::this_thread::sleep_for(kEdgeWaitSlice);
  }
}

void LiveDataSource::Abort() {
  async_.SetAborted();
  {
    base::AutoLock scoped(lock_);
    data_cv_.Broadcast();
  }
}

bool LiveDataSource::GetSize(int64_t* size_out) {
  // The size grows as the producer appends; reported honestly, but a live
  // consumer should rely on short reads, not on polling the size.
  base::AutoLock scoped(lock_);
  *size_out = static_cast<int64_t>(buffer_.size());
  return true;
}

bool LiveDataSource::IsStreaming() const {
  return true;
}

void LiveDataSource::SetBitrate(int /*bitrate*/) {}

bool LiveDataSource::IsSeekable() const {
  return false;  // The defining property of the live case.
}

void LiveDataSource::Read(int64_t offset, size_t size, uint8_t* data,
                          base::scoped_refptr<base::TaskRunner> task_runner,
                          ReadCB read_cb) {
  async_.Read(offset, size, data, std::move(task_runner), std::move(read_cb));
}

}  // namespace xmedia::media

// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/slice_buffer_source.h"

#include <algorithm>
#include <cstring>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"

namespace xmedia::media {
namespace {

base::scoped_refptr<DataSource>
CheckedInner(base::scoped_refptr<DataSource> inner) {
  CHECK(inner) << "SliceBufferSource needs an inner DataSource";
  return inner;
}

}  // namespace

SliceBufferSource::SliceBufferSource(base::scoped_refptr<DataSource> inner,
                                     Config config)
    : inner_(CheckedInner(std::move(inner))),
      config_(config),
      async_("xmedia-slice-src",
             base::BindRepeating(&SliceBufferSource::ReadBlocking,
                                 base::Unretained(this))) {}

SliceBufferSource::~SliceBufferSource() {
  async_.SetAborted();
  inner_->Abort();
  async_.StopWorker();
}

size_t SliceBufferSource::cached_bytes() const {
  std::lock_guard<std::mutex> scoped(lock_);
  return cached_bytes_;
}

void SliceBufferSource::SetHost(Host* host) {
  inner_->SetHost(host);
}

void SliceBufferSource::Read(int64_t offset, size_t size, uint8_t* data,
                             base::scoped_refptr<base::TaskRunner> task_runner,
                             ReadCB read_cb) {
  async_.Read(offset, size, data, std::move(task_runner), std::move(read_cb));
}

void SliceBufferSource::Abort() {
  async_.SetAborted();
  inner_->Abort();
}

bool SliceBufferSource::GetSize(int64_t* size_out) {
  return inner_->GetSize(size_out);
}

bool SliceBufferSource::IsStreaming() const {
  return inner_->IsStreaming();
}

void SliceBufferSource::SetBitrate(int bitrate) {
  inner_->SetBitrate(bitrate);
}

bool SliceBufferSource::IsSeekable() const {
  return inner_->IsSeekable();
}

size_t SliceBufferSource::CopyFromCacheLocked(int64_t offset, size_t size,
                                              uint8_t* data) {
  size_t copied = 0;
  while (copied < size) {
    const int64_t here = offset + static_cast<int64_t>(copied);
    const int64_t slice_start =
        (here / static_cast<int64_t>(config_.slice_size)) *
        static_cast<int64_t>(config_.slice_size);
    auto it = slices_.find(slice_start);
    if (it == slices_.end()) {
      break;
    }
    // Refresh LRU position for the touched slice.
    lru_.remove(slice_start);
    lru_.push_front(slice_start);
    const std::vector<uint8_t>& slice = it->second;
    const size_t in_slice = static_cast<size_t>(here - slice_start);
    const size_t available =
        slice.size() > in_slice ? slice.size() - in_slice : 0;
    if (available == 0) {
      break;
    }
    const size_t take = std::min(available, size - copied);
    memcpy(data + copied, slice.data() + in_slice, take);
    copied += take;
  }
  return copied;
}

DataSource::ReadResult SliceBufferSource::LoadSliceLocked(int64_t slice_start) {
  // Already cached (including a slice that came up short at EOF): touching
  // the inner source again would be waste AND would double-count
  // cached_bytes_ on the overwrite, so hand back what is stored.
  auto existing = slices_.find(slice_start);
  if (existing != slices_.end()) {
    lru_.remove(slice_start);
    lru_.push_front(slice_start);
    return static_cast<int>(existing->second.size());
  }
  std::vector<uint8_t> slice(config_.slice_size);
  size_t filled = 0;
  while (filled < config_.slice_size) {
    if (async_.IsAborted()) {
      return Err(ErrorCode::kAborted, "the slice source was aborted",
                 "slice start = " + std::to_string(slice_start),
                 "this follows Stop(); no action needed");
    }
    auto result = inner_->ReadBlocking(
        slice_start + static_cast<int64_t>(filled), config_.slice_size - filled,
        slice.data() + filled);
    if (!result.has_value()) {
      return result;
    }
    if (*result == 0) {
      break;  // EOF mid-slice: store the short slice as-is.
    }
    filled += static_cast<size_t>(*result);
  }
  slice.resize(filled);

  // Evict least-recently-used slices until the new one fits.
  while (cached_bytes_ + filled > config_.max_bytes && !lru_.empty()) {
    const int64_t victim = lru_.back();
    lru_.pop_back();
    auto it = slices_.find(victim);
    if (it != slices_.end()) {
      cached_bytes_ -= it->second.size();
      slices_.erase(it);
    }
  }
  if (filled > 0 && cached_bytes_ + filled <= config_.max_bytes) {
    slices_[slice_start] = std::move(slice);
    lru_.push_front(slice_start);
    cached_bytes_ += filled;
  }
  return static_cast<int>(filled);
}

DataSource::ReadResult
SliceBufferSource::ReadBlocking(int64_t offset, size_t size, uint8_t* data) {
  if (size == 0) {
    return 0;
  }
  std::lock_guard<std::mutex> scoped(lock_);
  size_t copied = CopyFromCacheLocked(offset, size, data);
  while (copied < size) {
    const int64_t here = offset + static_cast<int64_t>(copied);
    const int64_t slice_start =
        (here / static_cast<int64_t>(config_.slice_size)) *
        static_cast<int64_t>(config_.slice_size);
    auto result = LoadSliceLocked(slice_start);
    if (!result.has_value()) {
      if (copied > 0) {
        return static_cast<int>(copied);
      }
      return result;
    }
    if (*result == 0) {
      // EOF: a partial read before it is still a complete answer.
      if (copied > 0) {
        return static_cast<int>(copied);
      }
      return 0;
    }
    const size_t added =
        CopyFromCacheLocked(here, size - copied, data + copied);
    if (added == 0) {
      // The loaded slice cannot serve |here|: it came up short at EOF, or
      // the budget refused to store it. Serve the rest straight from the
      // inner source instead of looping on a slice that will never grow.
      auto rest = inner_->ReadBlocking(offset + static_cast<int64_t>(copied),
                                       size - copied, data + copied);
      if (!rest.has_value()) {
        if (copied > 0) {
          return static_cast<int>(copied);
        }
        return rest;
      }
      return static_cast<int>(copied + static_cast<size_t>(*rest));
    }
    copied += added;
  }
  return static_cast<int>(copied);
}

}  // namespace xmedia::media

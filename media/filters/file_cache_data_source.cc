// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/file_cache_data_source.h"

#include <cstdio>
#include <filesystem>
#include <string>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"

namespace avbase::media {
namespace {

base::scoped_refptr<DataSource>
CheckedInner(base::scoped_refptr<DataSource> inner) {
  CHECK(inner) << "FileCacheDataSource needs an inner DataSource";
  return inner;
}

// FNV-1a over the URL: stable across sessions, cheap, and collision risk is
// irrelevant for a per-URL file name (a collision only shares a cache file,
// which is then wrong-content guarded by nothing -- acceptable for names
// derived from full URIs including host and path).
std::string CacheFileName(const std::string& url) {
  uint64_t hash = 1469598103934665603ULL;
  for (char c : url) {
    hash ^= static_cast<uint8_t>(c);
    hash *= 1099511628211ULL;
  }
  char buffer[32];
  snprintf(buffer, sizeof(buffer), "%016llx",
           static_cast<unsigned long long>(hash));
  return std::string(buffer) + ".avcache";
}

// Creates the cache directory (best effort, recursive). std::filesystem's
// error_code overloads only: the tree builds with -fno-exceptions, so the
// throwing overloads are off the table.
void EnsureDirectory(const std::string& dir) {
  if (dir.empty()) {
    return;
  }
  std::error_code ignored;
  std::filesystem::create_directories(dir, ignored);
}

}  // namespace

FileCacheDataSource::FileCacheDataSource(base::scoped_refptr<DataSource> inner,
                                         Config config)
    : inner_(CheckedInner(std::move(inner))),
      config_(std::move(config)),
      async_("avbase-file-cache",
             base::BindRepeating(&FileCacheDataSource::ReadBlocking,
                                 base::Unretained(this))) {
  std::lock_guard<std::mutex> scoped(lock_);
  OpenCacheFileLocked();
}

FileCacheDataSource::~FileCacheDataSource() {
  async_.SetAborted();
  inner_->Abort();
  async_.StopWorker();
  std::lock_guard<std::mutex> scoped(lock_);
  if (cache_file_ != nullptr) {
    fclose(static_cast<FILE*>(cache_file_));
    cache_file_ = nullptr;
  }
}

int64_t FileCacheDataSource::cached_bytes() const {
  std::lock_guard<std::mutex> scoped(lock_);
  return downloaded_mark_;
}

bool FileCacheDataSource::cache_active() const {
  std::lock_guard<std::mutex> scoped(lock_);
  return cache_active_;
}

void FileCacheDataSource::OpenCacheFileLocked() {
  if (config_.cache_dir.empty()) {
    return;
  }
  EnsureDirectory(config_.cache_dir);
  const std::string path =
      (std::filesystem::path(config_.cache_dir) / CacheFileName(config_.url))
          .string();
  FILE* file = fopen(path.c_str(), "r+b");
  if (file == nullptr) {
    file = fopen(path.c_str(), "w+b");
  }
  if (file == nullptr) {
    return;  // read-through only; playback is never blocked by this.
  }
  fseek(file, 0, SEEK_END);
  const long size = ftell(file);
  downloaded_mark_ = static_cast<int64_t>(size);
  cache_file_ = file;
  cache_active_ = true;
}

void FileCacheDataSource::SetHost(Host* host) {
  inner_->SetHost(host);
}

void FileCacheDataSource::Read(
    int64_t offset, size_t size, uint8_t* data,
    base::scoped_refptr<base::TaskRunner> task_runner, ReadCB read_cb) {
  async_.Read(offset, size, data, std::move(task_runner), std::move(read_cb));
}

void FileCacheDataSource::Abort() {
  async_.SetAborted();
  inner_->Abort();
}

bool FileCacheDataSource::GetSize(int64_t* size_out) {
  return inner_->GetSize(size_out);
}

bool FileCacheDataSource::IsStreaming() const {
  return inner_->IsStreaming();
}

void FileCacheDataSource::SetBitrate(int bitrate) {
  inner_->SetBitrate(bitrate);
}

bool FileCacheDataSource::IsSeekable() const {
  return inner_->IsSeekable();
}

size_t FileCacheDataSource::ReadFromCacheLocked(int64_t offset, size_t size,
                                                uint8_t* data) {
  if (cache_file_ == nullptr || offset >= downloaded_mark_) {
    return 0;
  }
  const size_t available = static_cast<size_t>(downloaded_mark_ - offset);
  const size_t take = available < size ? available : size;
  fseek(static_cast<FILE*>(cache_file_), static_cast<long>(offset), SEEK_SET);
  const size_t got = fread(data, 1, take, static_cast<FILE*>(cache_file_));
  if (got < take) {
    // A short read inside the mark means the file cannot be trusted as a
    // contiguous prefix any more; degrade to read-through rather than let a
    // later append write over the hole.
    fclose(static_cast<FILE*>(cache_file_));
    cache_file_ = nullptr;
    cache_active_ = false;
  }
  return got;
}

void FileCacheDataSource::AppendToCacheLocked(const uint8_t* data,
                                              size_t bytes) {
  if (cache_file_ == nullptr || bytes == 0) {
    return;
  }
  if (static_cast<size_t>(downloaded_mark_) + bytes > config_.max_bytes) {
    return;  // budget reached: read-through only from here on.
  }
  fseek(static_cast<FILE*>(cache_file_), static_cast<long>(downloaded_mark_),
        SEEK_SET);
  const size_t wrote = fwrite(data, 1, bytes, static_cast<FILE*>(cache_file_));
  if (wrote == bytes) {
    fflush(static_cast<FILE*>(cache_file_));
    downloaded_mark_ += static_cast<int64_t>(bytes);
  } else {
    // The cache file is damaged or full: drop it from the path entirely
    // rather than serving a prefix we cannot trust.
    fclose(static_cast<FILE*>(cache_file_));
    cache_file_ = nullptr;
    cache_active_ = false;
  }
}

DataSource::ReadResult
FileCacheDataSource::ReadBlocking(int64_t offset, size_t size, uint8_t* data) {
  if (size == 0) {
    return 0;
  }
  std::lock_guard<std::mutex> scoped(lock_);
  size_t served = ReadFromCacheLocked(offset, size, data);
  while (served < size) {
    const int64_t here = offset + static_cast<int64_t>(served);
    // An append above may have grown the mark over |here| (the resume
    // path); re-check before paying the network for bytes on disk.
    const size_t from_cache =
        ReadFromCacheLocked(here, size - served, data + served);
    if (from_cache > 0) {
      served += from_cache;
      continue;
    }
    auto result = inner_->ReadBlocking(here, size - served, data + served);
    if (!result.has_value()) {
      if (served > 0) {
        return static_cast<int>(served);
      }
      return result;
    }
    if (*result == 0) {
      if (served > 0) {
        return static_cast<int>(served);
      }
      return 0;
    }
    AppendToCacheLocked(data + served, static_cast<size_t>(*result));
    served += static_cast<size_t>(*result);
  }
  return static_cast<int>(served);
}

}  // namespace avbase::media

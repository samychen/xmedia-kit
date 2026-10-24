// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/retry_data_source.h"

#include <chrono>
#include <utility>

#include "base/functional/bind.h"
#include "base/time/time.h"

namespace avbase::media {
namespace {

// Validates |inner| in the member-initialiser list, i.e. BEFORE |async_| is
// constructed. AsyncDataSourceBase starts its worker in its constructor, so a
// CHECK in the constructor BODY would run only after a thread already exists on
// a half-constructed object; doing it here keeps the original order
// ("check the inner source, then start the worker").
base::scoped_refptr<DataSource>
CheckedInner(base::scoped_refptr<DataSource> inner) {
  CHECK(inner) << "RetryDataSource needs an inner DataSource";
  return inner;
}

}  // namespace

RetryDataSource::RetryDataSource(base::scoped_refptr<DataSource> inner,
                                 Config config)
    : inner_(CheckedInner(std::move(inner))),
      config_(config),
      async_("avbase-retry-src",
             base::BindRepeating(&RetryDataSource::ReadBlocking,
                                 base::Unretained(this))) {}

RetryDataSource::~RetryDataSource() {
  // Unblock any in-flight retry loop (aborted_) and the inner read before the
  // worker is joined.
  async_.SetAborted();
  inner_->Abort();
  async_.StopWorker();
}

void RetryDataSource::SetHost(Host* host) {
  inner_->SetHost(host);
}

bool RetryDataSource::GetSize(int64_t* size_out) {
  return inner_->GetSize(size_out);
}

bool RetryDataSource::IsStreaming() const {
  return inner_->IsStreaming();
}

void RetryDataSource::SetBitrate(int bitrate) {
  inner_->SetBitrate(bitrate);
}

bool RetryDataSource::IsSeekable() const {
  return inner_->IsSeekable();
}

void RetryDataSource::Abort() {
  async_.SetAborted();
  inner_->Abort();
}

DataSource::ReadResult
RetryDataSource::ReadBlocking(int64_t offset, size_t size, uint8_t* data) {
  int failures = 0;
  while (true) {
    if (async_.IsAborted()) {
      return Err(ErrorCode::kAborted, "the retry source was aborted",
                 "offset = " + std::to_string(offset),
                 "this follows Stop(); no action needed");
    }
    ReadResult result = inner_->ReadBlocking(offset, size, data);
    if (result.has_value()) {
      return result;
    }
    // A success resets the streak; only consecutive failures spend the
    // budget, so intermittent blips never turn into a give-up.
    if (++failures > config_.max_retries) {
      return result;
    }
    const base::TimeTicks deadline =
        base::TimeTicks::Now() + config_.retry_delay;
    while (base::TimeTicks::Now() < deadline) {
      if (async_.IsAborted()) {
        return Err(ErrorCode::kAborted, "the retry source was aborted",
                   "offset = " + std::to_string(offset),
                   "this follows Stop(); no action needed");
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
}

void RetryDataSource::Read(int64_t offset, size_t size, uint8_t* data,
                           base::scoped_refptr<base::TaskRunner> task_runner,
                           ReadCB read_cb) {
  async_.Read(offset, size, data, std::move(task_runner), std::move(read_cb));
}

}  // namespace avbase::media

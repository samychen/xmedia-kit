// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/ffmpeg/url_data_source.h"

#include <mutex>
#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "media/ffmpeg/av_includes.h"
#include "media/ffmpeg/compat.h"

namespace avbase::media::ffmpeg {
namespace ff = ::avbase::media::ffmpeg;
namespace {

// The avio interrupt callback runs on the thread inside avio read/seek; it
// returns 1 to abort the operation in progress. This is what makes Abort()
// bound a blocked network read (docs/04 §5.4, Δ15) instead of waiting for
// the socket timeout.
int InterruptCallback(void* opaque) {
  auto* self = static_cast<UrlDataSource*>(opaque);
  return self->IsAbortedForInterrupt() ? 1 : 0;
}

media::MediaError ReadError(int av_error, const std::string& uri) {
  return media::MediaError(
      media::ErrorCode::kSourceReadFailed,
      "cannot read from the network source",
      "avio read on \"" + uri + "\" failed: " + ff::AvErrorString(av_error),
      "for a network source check connectivity and "
      "config.net.reconnect_max_retries, or call ReconnectNow()");
}

}  // namespace

UrlDataSource::UrlDataSource(std::string uri)
    : uri_(std::move(uri)),
      async_("avbase-url-src", base::BindRepeating(&UrlDataSource::ReadBlocking,
                                                   base::Unretained(this))) {}

UrlDataSource::~UrlDataSource() {
  // Unblock the in-flight avio call first (the interrupt callback polls the
  // abort flag), then join the worker, then close the context -- the reverse
  // of opening, and the same order LiveDataSource/RetryDataSource use through
  // the shared base.
  Abort();
  async_.StopWorker();
  std::scoped_lock scoped(lock_);
  CloseLocked();
}

media::DataSource::ReadResult UrlDataSource::EnsureOpen() {
  // Called with |lock_| held.
  if (avio_) {
    return 0;
  }
  AVIOContext* avio = nullptr;
  AVDictionary* opts = nullptr;
  // The interrupt callback is installed at OPEN time too: a connect against a
  // black-holed address must be abortable, not just a mid-stream read.
  AVIOInterruptCB cb = {&InterruptCallback, this};
  const int ret = avio_open2(&avio, uri_.c_str(), AVIO_FLAG_READ, &cb, &opts);
  av_dict_free(&opts);
  if (ret < 0) {
    return base::unexpected(media::MediaError(
        media::ErrorCode::kSourceOpenFailed, "cannot open the network source",
        "avio_open2(\"" + uri_ + "\"): " + ff::AvErrorString(ret),
        "check the URL and connectivity; connection failures are retried "
        "by config.net.reconnect when the retry layer is active"));
  }
  avio_ = avio;
  const int64_t size = avio_size(avio_);
  size_known_ = size > 0;
  size_ = size_known_ ? size : -1;
  seekable_ = (avio_->seekable & AVIO_SEEKABLE_NORMAL) != 0;
  return 0;
}

void UrlDataSource::CloseLocked() {
  if (avio_) {
    avio_closep(&avio_);
    avio_ = nullptr;
  }
}

void UrlDataSource::SetHost(Host* host) {
  // The bridge is the only consumer and does not register a host; the hook
  // exists for future backpressure ("buffer more before playing").
  (void)host;
}

media::DataSource::ReadResult
UrlDataSource::ReadBlocking(int64_t offset, size_t size, uint8_t* data) {
  if (size == 0) {
    return 0;
  }
  std::scoped_lock scoped(lock_);
  if (async_.IsAborted()) {
    return base::unexpected(media::MediaError(
        media::ErrorCode::kAborted, "the network source was aborted",
        "offset = " + std::to_string(offset),
        "this follows Stop(); no action needed"));
  }
  if (const ReadResult open = EnsureOpen(); !open) {
    return open;
  }
  // A seek per read matches how the DataSourceIO bridge drives the source
  // (next_offset_ bookkeeping is the bridge's; ours is stateless).
  const int64_t seek = avio_seek(avio_, offset, SEEK_SET);
  if (seek < 0) {
    return base::unexpected(ReadError(static_cast<int>(seek), uri_));
  }
  size_t total = 0;
  while (total < size) {
    if (async_.IsAborted()) {
      return base::unexpected(media::MediaError(
          media::ErrorCode::kAborted, "the network source was aborted",
          "offset = " + std::to_string(offset + static_cast<int64_t>(total)),
          "this follows Stop(); no action needed"));
    }
    const int got =
        avio_read(avio_, data + total, static_cast<int>(size - total));
    if (got == AVERROR_EOF) {
      break;  // Short read / clean EOF: return what we have.
    }
    if (got < 0) {
      return base::unexpected(ReadError(got, uri_));
    }
    total += static_cast<size_t>(got);
  }
  return static_cast<int>(total);
}

void UrlDataSource::Read(int64_t offset, size_t size, uint8_t* data,
                         base::scoped_refptr<base::TaskRunner> task_runner,
                         ReadCB read_cb) {
  // The DataSource contract says the callback is never run inline: the job
  // (blocking avio call included) runs on the worker, and only the RESULT is
  // posted to |task_runner|.
  //
  // The hand-written loop this replaced created its std::thread lazily under a
  // `if (!worker_)` test taken OUTSIDE the queue lock, so two concurrent
  // Read() calls could each start a thread and strand one of them unjoined for
  // the process's lifetime. The shared base starts its worker in its
  // constructor, so that window does not exist.
  async_.Read(offset, size, data, std::move(task_runner), std::move(read_cb));
}

void UrlDataSource::Abort() {
  // One flag, owned by the shared base, serving all three readers: the avio
  // interrupt callback (via IsAbortedForInterrupt, so a blocked connect or
  // read returns promptly), ReadBlocking's own guard, and the base's
  // "torn down" reply for reads queued after this point. A second copy here
  // is what the previous version kept, and two flags can disagree.
  async_.SetAborted();
}

bool UrlDataSource::GetSize(int64_t* size_out) {
  std::scoped_lock scoped(lock_);
  if (const ReadResult open = EnsureOpen(); !open) {
    return false;
  }
  if (!size_known_) {
    return false;
  }
  if (size_out) {
    *size_out = size_;
  }
  return true;
}

bool UrlDataSource::IsStreaming() const {
  // Unknown size == cannot bound the download == streaming semantics.
  std::scoped_lock scoped(lock_);
  return !size_known_;
}

void UrlDataSource::SetBitrate(int bitrate) {
  // Interface override only: nothing downstream reads a bitrate hint (the
  // demuxer measures throughput itself), so there is no field to write.
  (void)bitrate;
}

bool UrlDataSource::IsSeekable() const {
  std::scoped_lock scoped(lock_);
  return seekable_;
}

}  // namespace avbase::media::ffmpeg

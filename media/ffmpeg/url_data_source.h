// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FFMPEG_URL_DATA_SOURCE_H_
#define AVBASE_MEDIA_FFMPEG_URL_DATA_SOURCE_H_

#include <cstdint>
#include <mutex>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "base/task/task_runner.h"
#include "media/base/data_source.h"
#include "media/base/media_error.h"
#include "media/filters/async_data_source_base.h"
#include "media/media_export.h"

struct AVIOContext;

namespace avbase::media::ffmpeg {

// An avio-backed DataSource for network URIs (http/https today). This is the
// component that lets the RETRY layer exist on the network path at all: the
// FFmpeg protocol layer can reconnect itself mid-stream, but a connection
// reset during connect or a hard reset mid-transfer surfaces as a read error
// on OUR side -- and only a DataSource decorator (RetryDataSource) wrapping
// THIS can turn that into a fresh connection. Before this existed,
// RetryDataSource was implemented, tested, and wired to nothing.
//
// LIFETIME/THREADING: ReadBlocking runs on the demuxer's thread and may block
// for the network duration; Abort() flips the shared abort flag that the avio
// interrupt callback observes, so a blocked read returns AVERROR_EXIT promptly
// (Δ15). The async Read() runs on a private worker like RetryDataSource's: the
// callback is posted to |task_runner|, never inline.
class AVBASE_MEDIA_EXPORT UrlDataSource final : public media::DataSource {
 public:
  explicit UrlDataSource(std::string uri);
  UrlDataSource(const UrlDataSource&) = delete;
  UrlDataSource& operator=(const UrlDataSource&) = delete;
  ~UrlDataSource() override;

  // media::DataSource:
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

  // Observed by the avio interrupt callback (registered at open). Public
  // only because the callback is a file-local free function.
  bool IsAbortedForInterrupt() const { return async_.IsAborted(); }

 private:
  // Opens the AVIOContext on first use (the demuxer opens its probe on its
  // own thread; opening here keeps the ctor infallible and lets open errors
  // surface as read errors with the URI in the message).
  media::DataSource::ReadResult EnsureOpen();
  void CloseLocked();

  const std::string uri_;
  mutable std::mutex lock_;
  AVIOContext* avio_ = nullptr;  // GUARDED_BY(lock_) for the pointer itself.
  bool size_known_ = false;      // GUARDED_BY(lock_)
  int64_t size_ = -1;            // GUARDED_BY(lock_)
  bool seekable_ = false;        // GUARDED_BY(lock_)

  // Shared async-Read plumbing (worker, job queue, abort flag). Declared last
  // so lock_/avio_ are fully constructed before its worker can run
  // ReadBlocking, and bound to ReadBlocking directly: a URL read has no
  // separate "wait at the edge" half to keep apart. This replaced a third
  // hand-written copy of the same worker loop -- see async_data_source_base.h
  // for what the three now share.
  AsyncDataSourceBase async_;
};

}  // namespace avbase::media::ffmpeg

#endif  // AVBASE_MEDIA_FFMPEG_URL_DATA_SOURCE_H_

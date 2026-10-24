// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FILTERS_ASYNC_DATA_SOURCE_BASE_H_
#define AVBASE_MEDIA_FILTERS_ASYNC_DATA_SOURCE_BASE_H_

#include <deque>

#include "base/functional/callback.h"
#include "base/memory/scoped_refptr.h"
#include "base/synchronization/atomic_flag.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/task/task_runner.h"
#include "base/threading/thread.h"
#include "media/base/data_source.h"
#include "media/base/media_error.h"

namespace avbase::media {

// Owns the async-Read plumbing shared by LiveDataSource and RetryDataSource: a
// worker thread that drains a queue of (offset, size, data, runner, cb) jobs
// and posts each result back on the caller's task runner -- never inline, per
// the DataSource contract (docs/03 §4). The two sources differed only in what
// |blocking_read| does on the worker (live: wait at the edge; retry: loop with
// backoff); that is the single piece each owner supplies.
//
// COMPOSITION, NOT INHERITANCE: an owner holds one of these as a member,
// constructs it with a thread name and a repeating callback bound to its own
// blocking Read, forwards DataSource::Read to Read(), and calls StopWorker()
// from its destructor (after unblocking any in-flight blocking read via its own
// Abort()/Close()). That is why this class has no virtual methods and its
// destructor is not virtual.
class AsyncDataSourceBase {
 public:
  // Invoked on the worker thread for every queued Read(). It must not call back
  // into the async Read() path; it is the synchronous, possibly blocking half
  // of the contract.
  using BlockingRead = base::RepeatingCallback<DataSource::ReadResult(
      int64_t offset, size_t size, uint8_t* data)>;

  AsyncDataSourceBase(const char* worker_name, BlockingRead blocking_read);
  ~AsyncDataSourceBase();

  // Enqueue an async Read. Never runs |read_cb| inline; if the worker is
  // already stopped, posts an abort error on |task_runner| immediately.
  void Read(int64_t offset, size_t size, uint8_t* data,
            base::scoped_refptr<base::TaskRunner> task_runner,
            DataSource::ReadCB read_cb);

  // Stops the worker thread. Idempotent; the owner calls it from its
  // destructor after unblocking any in-flight blocking read.
  void StopWorker();

  // Abort flag. The owner sets it from its Abort() and polls IsAborted() from
  // the blocking read, which must return promptly once set so the worker can
  // drain and exit.
  void SetAborted() { aborted_.Set(); }
  bool IsAborted() const { return aborted_.IsSet(); }

 private:
  // The queued read request. Built by Read() and consumed by WorkerMain, so it
  // is deliberately invisible to the owner.
  struct AsyncJob {
    int64_t offset{0};
    size_t size{0};
    uint8_t* data{nullptr};
    base::scoped_refptr<base::TaskRunner> runner;
    DataSource::ReadCB cb;
  };

  void WorkerMain();

  BlockingRead blocking_read_;
  base::AtomicFlag aborted_;
  base::Thread worker_;
  base::Lock job_lock_;
  base::ConditionVariable job_cv_;
  std::deque<AsyncJob> jobs_ GUARDED_BY(job_lock_);
  bool stopped_ GUARDED_BY(job_lock_) = false;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FILTERS_ASYNC_DATA_SOURCE_BASE_H_

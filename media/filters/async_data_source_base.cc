// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/filters/async_data_source_base.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "media/base/media_error.h"

namespace avbase::media {

AsyncDataSourceBase::AsyncDataSourceBase(const char* worker_name,
                                         BlockingRead blocking_read)
    : blocking_read_(std::move(blocking_read)),
      worker_(worker_name),
      job_cv_(&job_lock_) {
  worker_.Start();
  worker_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&AsyncDataSourceBase::WorkerMain, base::Unretained(this)));
}

AsyncDataSourceBase::~AsyncDataSourceBase() {
  StopWorker();
}

void AsyncDataSourceBase::StopWorker() {
  {
    base::AutoLock scoped(job_lock_);
    if (stopped_) {
      return;
    }
    stopped_ = true;
  }
  // Wake a possibly-idle worker so it observes stopped_ and exits, then join.
  job_cv_.Signal();
  worker_.Stop();
}

void AsyncDataSourceBase::Read(
    int64_t offset, size_t size, uint8_t* data,
    base::scoped_refptr<base::TaskRunner> task_runner,
    DataSource::ReadCB read_cb) {
  base::AutoLock scoped(job_lock_);
  if (stopped_) {
    task_runner->PostTask(
        FROM_HERE,
        base::BindOnce(std::move(read_cb),
                       Err(ErrorCode::kAborted, "the source was torn down", {},
                           "no action needed")));
    return;
  }
  jobs_.push_back(
      AsyncJob{offset, size, data, std::move(task_runner), std::move(read_cb)});
  job_cv_.Signal();
}

void AsyncDataSourceBase::WorkerMain() {
  while (true) {
    AsyncJob job;
    {
      base::AutoLock scoped(job_lock_);
      while (jobs_.empty() && !stopped_) {
        job_cv_.Wait();
      }
      if (stopped_) {
        return;
      }
      job = std::move(jobs_.front());
      jobs_.pop_front();
    }
    DataSource::ReadResult result =
        blocking_read_.Run(job.offset, job.size, job.data);
    job.runner->PostTask(FROM_HERE,
                         base::BindOnce(std::move(job.cb), std::move(result)));
  }
}

}  // namespace avbase::media

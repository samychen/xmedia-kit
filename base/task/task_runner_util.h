// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `base/task/task_runner_util.h` (BSD-3-Clause).

#ifndef AVBASE_BASE_TASK_TASK_RUNNER_UTIL_H_
#define AVBASE_BASE_TASK_TASK_RUNNER_UTIL_H_

#include <type_traits>
#include <utility>

#include "base/functional/bind.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_runner.h"

namespace avbase::base {

// Runs |task| on |task_runner| and delivers its return value to |reply| on the
// calling sequence. This is how an async media API reports a result without the
// caller having to hand-roll two callbacks.
//
// Returns false if the task could not be posted (runner shutting down), in
// which case |reply| never runs — callers must treat a false return as "no
// result will arrive".
template <typename TaskReturnType, typename ReplyArgType, typename Task,
          typename Reply>
bool PostTaskAndReplyWithResult(TaskRunner* task_runner,
                                const Location& from_here, Task&& task,
                                Reply&& reply) {
  static_assert(std::is_convertible_v<TaskReturnType, ReplyArgType>,
                "task return type must be convertible to the reply argument");

  // The reply runs on the *calling sequence*, matching Chromium's
  // PostTaskAndReplyWithResult / PostTaskAndReply: |task| executes on
  // |task_runner| (the worker), but the result is delivered back where the
  // async call was issued. avbase media objects are only ever touched on their
  // own sequence (docs/04 §3), so redirecting the reply onto |task_runner|
  // would let a different sequence reach into them. Capture the caller's
  // sequence now, before |task| is posted and possibly starts running.
  scoped_refptr<SequencedTaskRunner> reply_runner =
      SequencedTaskRunner::GetCurrentDefault();

  // Shared slot carrying the result from the worker sequence back. Refcounted
  // so the reply still has somewhere to read from even if the poster is gone.
  struct ResultSlot : public RefCountedThreadSafe<ResultSlot> {
    TaskReturnType value{};

   private:
    friend class RefCountedThreadSafe<ResultSlot>;
    ~ResultSlot() = default;
  };

  auto slot = MakeRefCounted<ResultSlot>();

  // Plain capturing lambdas rather than BindOnce: the bound arguments here are
  // move-only (a OnceCallback reply, a Task functor), and a capture list moves
  // them without going through a std::tuple of bound args.
  OnceClosure work([t = std::forward<Task>(task), slot]() mutable {
    slot->value = std::move(t)();
  });
  if (!task_runner->PostTask(from_here, std::move(work))) {
    return false;
  }

  OnceClosure done([r = std::forward<Reply>(reply), slot]() mutable {
    std::move(r).Run(std::move(slot->value));
  });
  // Reply on the captured calling sequence. Fall back to |task_runner| only
  // when there is no current default sequence (e.g. called outside any
  // sequence), so the result is never silently dropped.
  if (reply_runner) {
    return reply_runner->PostTask(from_here, std::move(done));
  }
  return task_runner->PostTask(from_here, std::move(done));
}

}  // namespace avbase::base

#endif  // AVBASE_BASE_TASK_TASK_RUNNER_UTIL_H_

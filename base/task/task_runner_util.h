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
  // The reply runs on the *calling sequence*, captured now. When there is no
  // current default sequence (called outside any sequence) it is null, and the
  // reply is posted back onto |task_runner| so the result is never silently
  // dropped.
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

  // |value| is written by the task on |task_runner| and read by the reply on
  // |reply_runner|. When those are two DIFFERENT sequences the two accesses
  // race unless the reply is posted from *inside* the worker task: the reply
  // queue mutex (release on PostTask, acquire on run) is then the only thing
  // that orders the worker's write before the reply's read. When they are the
  // same sequence -- or there is no current default, so the reply falls back
  // onto |task_runner| -- both run on one thread, so the reply may be posted up
  // front, which also preserves the (ready_time, sequence_number) ordering the
  // queue relies on and the single-runner RunAllReadyTasks(now) drain pattern.
  const bool cross_sequence =
      reply_runner &&
      static_cast<TaskRunner*>(reply_runner.get()) != task_runner;

  if (!cross_sequence) {
    OnceClosure work([t = std::forward<Task>(task), slot]() mutable {
      slot->value = std::move(t)();
    });
    if (!task_runner->PostTask(from_here, std::move(work))) {
      return false;
    }
    OnceClosure done([r = std::forward<Reply>(reply), slot]() mutable {
      std::move(r).Run(std::move(slot->value));
    });
    if (reply_runner) {
      return reply_runner->PostTask(from_here, std::move(done));
    }
    return task_runner->PostTask(from_here, std::move(done));
  }

  // Cross-sequence: run the task on the worker, then post the reply to the
  // caller's sequence from inside the worker so the hand-off is synchronized.
  OnceClosure work([t = std::forward<Task>(task),
                    r = std::forward<Reply>(reply), reply_runner, slot,
                    from_here]() mutable {
    slot->value = std::move(t)();
    OnceClosure done([r = std::move(r), slot]() mutable {
      std::move(r).Run(std::move(slot->value));
    });
    reply_runner->PostTask(from_here, std::move(done));
  });
  return task_runner->PostTask(from_here, std::move(work));
}

}  // namespace avbase::base

#endif  // AVBASE_BASE_TASK_TASK_RUNNER_UTIL_H_

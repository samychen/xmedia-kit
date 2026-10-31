// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// Mirrors Chromium's `base/task/sequenced_task_runner.h` (BSD-3-Clause).

#ifndef XMEDIA_BASE_TASK_SEQUENCED_TASK_RUNNER_H_
#define XMEDIA_BASE_TASK_SEQUENCED_TASK_RUNNER_H_

#include "base/base_export.h"
#include "base/task/task_runner.h"

namespace xmedia::base {

// A TaskRunner whose tasks run one at a time in posting order.
//
// "Sequence" in xmedia-kit means exactly this: a serialised execution context.
// Most
// sequences are backed by a dedicated thread (base::Thread), but a sequence is
// not a thread — base::test::TaskEnvironment runs one inline on the test
// thread, which is what makes scheduling logic deterministically testable.
class XMEDIA_BASE_EXPORT SequencedTaskRunner : public TaskRunner {
 public:
  // The default runner for the calling sequence, or nullptr when it has none.
  // Set automatically by base::Thread and base::test::TaskEnvironment.
  static scoped_refptr<SequencedTaskRunner> GetCurrentDefault();
  static void SetCurrentDefault(scoped_refptr<SequencedTaskRunner> runner);
  static bool HasCurrentDefault();

 protected:
  SequencedTaskRunner();
  ~SequencedTaskRunner() override;
};

// Convenience alias matching Chromium usage where a sequence is pinned to one
// thread. xmedia-kit does not distinguish the two at the type level: every
// sequence
// here is single-threaded by construction.
using SingleThreadTaskRunner = SequencedTaskRunner;

}  // namespace xmedia::base

#endif  // XMEDIA_BASE_TASK_SEQUENCED_TASK_RUNNER_H_

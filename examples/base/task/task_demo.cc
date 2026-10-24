// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// task_demo.cc — 演示 base/task/ 的全部功能与用法。
//
// base/task 是 avbase 的「消息循环 / 任务投递」层，对应 Chromium 的
// base/task。它定义了三条核心契约：
//   1. TaskRunner      —— 投递任务的终点；PostTask 是跨序列触碰对象的
//                         唯一被认可的方式，配合 base::WeakPtr 能在结构上
//                         消灭「player 已释放、回调还在飞」那类崩溃。
//   2. SequencedTaskRunner —— 串行执行上下文（一个接一个、按投递序）；
//                         avbase 里每个序列在构造上都是单线程的。
//   3. TaskQueue       —— 最终的消息循环实现（消息泵 + 队列合一），
//                         生产路径由 base::Thread 在其专属线程上调 Run()。
//
// 运行方式（手动，针对已构建的 build/full；库里已含 DCHECK）：
//   clang++ -std=c++20 -DAVBASE_ENABLE_DCHECK -I. \
//       examples/base/task/task_demo.cc build/full/lib/libavbase_base.a \
//       -lpthread -o /tmp/task_demo
//   /tmp/task_demo
//
// 或通过 CMake（examples/CMakeLists.txt 已加入 task_demo 目标）：
//   cmake --build build/full --target task_demo
//   ./build/full/bin/task_demo
//
// 关于本 demo 的日志输出（LOG(INFO)）：
//   * 直接跑上面的二进制即可看到全部 [INFO ...] 输出——demo 是独立可执行程序，
//     LOG(INFO) 默认就写到 stderr，无需任何额外设置。
//   * 若把 demo 逻辑搬进 tests/unit 单测再跑，注意 ctest 默认只在测试「失败时」
//     才吐出 stderr，通过的测试日志会被缓冲而“看似没打”。此时要么直接跑
//     build/full/bin/<name>_unittests --gtest_filter=...，要么 ctest --output-on-failure。
//   * DLOG(INFO) 仅在 DCHECK 开启（debug / AVBASE_ENABLE_DCHECK）时打印，
//     release 构建整段塌空；VLOG(n) 默认关闭，需先 base::logging::SetMinVLogLevel(n)。
//
// 说明：为了让延迟调度、排序、FastForward 语义「确定可复现」，本 demo 用一个
// 受控的 ManualClock 取代真实时钟（这正对应 Chromium 的 SimpleTestTickClock /
// 设计原则 P6：调度逻辑必须可确定性测试，绝不 sleep）。Part 6 才用真实后台
// 线程演示生产路径。

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/task_queue.h"
#include "base/task/task_runner.h"
#include "base/task/task_runner_util.h"
#include "base/time/tick_clock.h"
#include "base/time/time.h"

namespace base = avbase::base;

// ---------------------------------------------------------------------------
// ManualClock —— 受控单调时钟，仅用于本 demo 的确定性驱动。
// 真实代码用 DefaultTickClock；测试用 SimpleTestTickClock（在 base/test/，属测试
// 支持库，examples 不依赖）。这里自己实现一个，语义一致：NowTicks() 返回我们手动
// 推进的时间，PostDelayedTask 据此计算 ready_time。
// ---------------------------------------------------------------------------
class ManualClock : public base::TickClock {
 public:
  explicit ManualClock(base::TimeTicks start) : now_(start) {}
  base::TimeTicks NowTicks() const override { return now_; }
  void Advance(base::TimeDelta d) { now_ = now_ + d; }
  base::TimeTicks now() const { return now_; }

 private:
  base::TimeTicks now_;
};

// 把一个 vector<int> 拼成 "0 1 2" 形式的字符串，方便打印执行顺序。
static std::string Join(const std::vector<int>& v) {
  std::string s;
  for (int x : v) {
    s += std::to_string(x);
    s += ' ';
  }
  return s;
}

// ===========================================================================
// Part 1 — PostTask 与顺序保证（SequencedTaskRunner 的核心语义）
// ===========================================================================
static void Part1_PostAndOrder() {
  LOG(INFO) << "===== Part 1: PostTask 与顺序保证（SequencedTaskRunner）=====";

  auto queue = base::MakeRefCounted<base::TaskQueue>();
  ManualClock clock(base::TimeTicks::FromMicroseconds(1'000'000));
  queue->SetTickClockForTesting(&clock);
  queue->BindToCurrentSequence();  // 把当前（主）线程标为这个 queue 的序列

  // (a) 顺序投递 5 个任务，它们会按投递顺序一个接一个串行执行。
  std::vector<int> order;
  for (int i = 0; i < 5; ++i) {
    CHECK(queue->PostTask(FROM_HERE,
                          base::BindOnce([&order, i]() { order.push_back(i); })));
  }
  LOG(INFO) << "(a) 投递 5 个任务后 GetPendingTaskCount() = "
            << queue->GetPendingTaskCount();

  // (b) 空 / 默认构造的 OnceClosure 会被拒绝（返回 false，不入队）。
  bool null_rejected = !queue->PostTask(FROM_HERE, base::OnceClosure());
  LOG(INFO) << "(b) 空 OnceClosure 被拒绝 = " << null_rejected;

  // (c) RunAllReadyTasks(now) 内联跑掉当前就绪的全部任务（此处全部即时）。
  //     PostTask 从不在投递线程「内联」运行——只有 Run()/RunAllReadyTasks 这类
  //     驱动的调用才会真正执行任务体。
  size_t ran = queue->RunAllReadyTasks(clock.now());
  LOG(INFO) << "(c) RunAllReadyTasks 执行了 " << ran
            << " 个；串行、按投递序执行顺序: " << Join(order);

  // (d) 跑完后队列清空。
  LOG(INFO) << "(d) 执行后 GetPendingTaskCount() = "
            << queue->GetPendingTaskCount();
}

// ===========================================================================
// Part 2 — PostDelayedTask 与排序（延迟调度）
// ===========================================================================
static void Part2_Delayed() {
  LOG(INFO) << "===== Part 2: PostDelayedTask 与排序（延迟调度）=====";

  auto queue = base::MakeRefCounted<base::TaskQueue>();
  ManualClock clock(base::TimeTicks::FromMicroseconds(1'000'000));
  queue->SetTickClockForTesting(&clock);
  queue->BindToCurrentSequence();

  // (a)(b)(c) 三个延迟任务：200ms、100ms，外加一个即时任务。
  std::vector<int> order;
  queue->PostDelayedTask(FROM_HERE,
                         base::BindOnce([&order]() { order.push_back(2); }),
                         base::Milliseconds(200));
  queue->PostDelayedTask(FROM_HERE,
                         base::BindOnce([&order]() { order.push_back(1); }),
                         base::Milliseconds(100));
  queue->PostTask(FROM_HERE,
                  base::BindOnce([&order]() { order.push_back(0); }));

  LOG(INFO) << "(a) t=0 只跑即时任务（延迟的还没到期）";
  queue->RunAllReadyTasks(clock.now());
  LOG(INFO) << "    顺序: " << Join(order);

  clock.Advance(base::Milliseconds(150));  // 现在 100ms 到期，200ms 未到期
  LOG(INFO) << "(b) 推进 150ms：到期 100ms 的任务执行";
  queue->RunAllReadyTasks(clock.now());
  LOG(INFO) << "    顺序: " << Join(order);

  clock.Advance(base::Milliseconds(150));  // 现在 200ms 也到期
  LOG(INFO) << "(c) 再推进 150ms：到期 200ms 的任务执行";
  queue->RunAllReadyTasks(clock.now());
  LOG(INFO) << "    最终顺序: " << Join(order);

  // (d) 负延迟被当作即时任务（不会报错，也不会真的往过去排）。
  auto q2 = base::MakeRefCounted<base::TaskQueue>();
  ManualClock c2(base::TimeTicks::FromMicroseconds(1'000'000));
  q2->SetTickClockForTesting(&c2);
  q2->BindToCurrentSequence();
  bool neg_ran = false;
  q2->PostDelayedTask(FROM_HERE,
                      base::BindOnce([&neg_ran]() { neg_ran = true; }),
                      base::Milliseconds(-500));
  q2->RunAllReadyTasks(c2.now());
  LOG(INFO) << "(d) 负延迟被当作即时任务执行 = " << neg_ran;

  // (e) 排序不变量：(ready_time, sequence_number)。一个「已到期」的延迟任务
  //     不该被「稍后投递」的即时任务超车——没有这条，FastForwardBy 就不确定。
  auto q3 = base::MakeRefCounted<base::TaskQueue>();
  ManualClock c3(base::TimeTicks::FromMicroseconds(1'000'000));
  q3->SetTickClockForTesting(&c3);
  q3->BindToCurrentSequence();
  std::vector<int> order3;
  q3->PostDelayedTask(FROM_HERE,
                      base::BindOnce([&order3]() { order3.push_back(1); }),
                      base::Milliseconds(10));
  c3.Advance(base::Milliseconds(20));  // 延迟任务已到期
  q3->PostTask(FROM_HERE,
               base::BindOnce([&order3]() { order3.push_back(2); }));  // 后投的即时
  q3->RunAllReadyTasks(c3.now());
  LOG(INFO) << "(e) 已到期延迟任务(1)不被后投即时任务(2)超车，顺序: "
            << Join(order3);
}

// ===========================================================================
// Part 3 — 诊断与序列 API
// ===========================================================================
static void Part3_Diagnostics() {
  LOG(INFO) << "===== Part 3: 诊断与序列 API =====";

  auto queue = base::MakeRefCounted<base::TaskQueue>();
  ManualClock clock(base::TimeTicks::FromMicroseconds(1'000'000));
  queue->SetTickClockForTesting(&clock);
  queue->BindToCurrentSequence();  // 当前线程现在就是这个 queue 的序列

  // (a) RunsTasksInCurrentSequence()：是否在「本 queue 运行的序列」上调用。
  LOG(INFO) << "(a) BindToCurrentSequence 后，主线程 RunsTasksInCurrentSequence() = "
            << queue->RunsTasksInCurrentSequence();

  // (b) GetNextReadyTime()：最早就绪时间；空队列返回 null。
  LOG(INFO) << "(b) 空队列 GetNextReadyTime().is_null() = "
            << queue->GetNextReadyTime().is_null();
  queue->PostDelayedTask(FROM_HERE, base::DoNothing(), base::Milliseconds(300));
  queue->PostDelayedTask(FROM_HERE, base::DoNothing(), base::Milliseconds(100));
  bool reports_earliest =
      (queue->GetNextReadyTime() == clock.now() + base::Milliseconds(100));
  LOG(INFO) << "(c) GetNextReadyTime() 报告最早到期项(100ms) = " << reports_earliest;

  // (d) 在任务体内部调用 RunsTasksInCurrentSequence() 同样为 true——这是「当前
  //     序列」判定的典型用途：一个对象要区分「我在自己的序列上被调用」vs
  //     「别人跨序列调我」。
  bool inside = false;
  queue->PostTask(FROM_HERE,
                  base::BindOnce([&queue, &inside]() {
                    inside = queue->RunsTasksInCurrentSequence();
                  }));
  queue->RunAllReadyTasks(clock.now());
  LOG(INFO) << "(d) 任务体内部 RunsTasksInCurrentSequence() = " << inside;
}

// ===========================================================================
// Part 4 — Clear() 丢弃未完成任务 + 引用环 RULE
// ===========================================================================
static void Part4_ClearAndCycleRule() {
  LOG(INFO) << "===== Part 4: Clear() 与「引用环」RULE =====";

  auto queue = base::MakeRefCounted<base::TaskQueue>();
  ManualClock clock(base::TimeTicks::FromMicroseconds(1'000'000));
  queue->SetTickClockForTesting(&clock);
  queue->BindToCurrentSequence();

  // (a)(b) Clear() 把尚未执行的任务全部丢弃，PostTask 返回 false 的情况之外，
  //       这是另一条「主动放弃 pending 任务」的路径。
  int runs = 0;
  queue->PostTask(FROM_HERE, base::BindOnce([&runs]() { ++runs; }));
  queue->PostDelayedTask(FROM_HERE, base::BindOnce([&runs]() { ++runs; }),
                         base::Seconds(1));
  LOG(INFO) << "(a) 投递 2 个任务后 GetPendingTaskCount() = "
            << queue->GetPendingTaskCount();
  queue->Clear();
  size_t after_clear = queue->RunAllReadyTasks(clock.now());
  LOG(INFO) << "(b) Clear() 后 GetPendingTaskCount() = "
            << queue->GetPendingTaskCount()
            << "，RunAllReadyTasks 返回 " << after_clear
            << "（runs 仍为 " << runs << "，未执行任务被丢弃）";

  // (c)(d)(e) RULE（task_queue.h 明示）：一个任务绝不能再持有所属 TaskQueue 的
  //     scoped_refptr——队列拥有任务，这是引用环，队列永不析构，LeakSanitizer 会
  //     真实报告（不是误报）。正确做法：用 base::WeakPtr 捕获，或在丢弃自己那份
  //     引用前 Clear()。下面刻意制造一个 reposting 环，再用 Clear() 化解，确保
  //     进程干净退出（不泄漏）。
  auto cyc = base::MakeRefCounted<base::TaskQueue>();
  ManualClock cc(base::TimeTicks::FromMicroseconds(1'000'000));
  cyc->SetTickClockForTesting(&cc);
  cyc->BindToCurrentSequence();

  struct Reposter {
    base::scoped_refptr<base::TaskQueue> q;  // 错误写法：task 持有所属队列的引用
    int* n;
    void operator()() {
      if (++(*n) < 5) {
        // 每次重投都带一份 q 的引用；延迟 1ms 让它在本次 RunAllReadyTasks 之外残留
        q->PostDelayedTask(FROM_HERE, base::BindOnce(Reposter{*this}),
                           base::Milliseconds(1));
      }
    }
  };
  int n = 0;
  cyc->PostTask(FROM_HERE, base::BindOnce(Reposter{cyc, &n}));
  cyc->RunAllReadyTasks(cc.now());  // 跑首轮，重投的延迟任务残留并持有 cyc 引用
  LOG(INFO) << "(c) RULE 演示：reposting 的 pending 持有 cyc 引用，GetPendingTaskCount() = "
            << cyc->GetPendingTaskCount();
  LOG(INFO) << "(d) 若不处理，cyc 因引用环永不析构 -> 泄漏。正确做法：丢引用前 Clear()。";
  cyc->Clear();  // 破环：丢弃所有 pending（连同它们持有的 cyc 引用）
  LOG(INFO) << "(e) Clear() 破环后 GetPendingTaskCount() = "
            << cyc->GetPendingTaskCount() << "（随后 cyc 析构不再泄漏）";
}

// ===========================================================================
// Part 5 — PostTaskAndReplyWithResult（task_runner_util）
// ===========================================================================
static void Part5_ReplyWithResult() {
  LOG(INFO) << "===== Part 5: PostTaskAndReplyWithResult（task_runner_util）=====";

  auto queue = base::MakeRefCounted<base::TaskQueue>();
  ManualClock clock(base::TimeTicks::FromMicroseconds(1'000'000));
  queue->SetTickClockForTesting(&clock);
  queue->BindToCurrentSequence();

  // (a)(b) 在 runner 上跑一个返回值的任务，并把结果通过 reply 回传——无需手搓
  //        两个回调。注意 task 与 reply 都跑在同一个 runner（此处即调用方序列）。
  int received = 0;
  bool ok = base::PostTaskAndReplyWithResult<int, int>(
      queue.get(), FROM_HERE, []() { return 42; },
      base::BindOnce([](int* out, int v) { *out = v; }, &received));
  LOG(INFO) << "(a) PostTaskAndReplyWithResult 投递成功 = " << ok;
  queue->RunAllReadyTasks(clock.now());
  LOG(INFO) << "(b) 结果从 worker 任务回传到 reply：received = " << received;

  // (c) runner 已 Quit（不再接受任务）时，返回 false 且 reply 不会运行——调用方
  //     必须把 false 当成「不会有结果送达」处理。
  auto q2 = base::MakeRefCounted<base::TaskQueue>();
  q2->Quit();
  int ignored = -1;
  bool rejected = !base::PostTaskAndReplyWithResult<int, int>(
      q2.get(), FROM_HERE, []() { return 1; },
      base::BindOnce([](int* out, int v) { *out = v; }, &ignored));
  LOG(INFO) << "(c) runner 已 Quit 时返回 false（reply 不会运行） = " << rejected
            << "，ignored 仍为 " << ignored;
}

// ===========================================================================
// Part 6 — 真实多线程 Run()/Quit()（生产路径，base::Thread 的用法）
// ===========================================================================
static void Part6_RealRunLoop() {
  LOG(INFO) << "===== Part 6: 真实多线程 Run()/Quit()（生产路径）=====";

  auto queue = base::MakeRefCounted<base::TaskQueue>();
  std::atomic<int> executed{0};
  // 后台线程调 Run()：阻塞消费，直到别的线程（或某个任务）调 Quit()。
  std::thread runner([&queue]() { queue->Run(); });

  // (a) 主线程跨线程投递 100 个任务；PostTask 是线程安全的。
  for (int i = 0; i < 100; ++i) {
    while (!queue->PostTask(
        FROM_HERE,
        base::BindOnce([&executed]() { executed.fetch_add(1); }))) {
      std::this_thread::yield();  // runner 关闭后才会失败，这里正常不会进
    }
  }
  for (int i = 0; i < 1000 && executed.load() < 100; ++i) {
    std::this_thread::sleep_for(base::Milliseconds(1).ToChronoMicros());
  }
  LOG(INFO) << "(a) 后台线程 Run() 消费了 " << executed.load()
            << " 个跨线程投递的任务";

  // (b) Quit() 线程安全：唤醒 Run() 并使其返回。之后再 PostTask 一律返回 false——
  //     这正是「调用方得知目标序列已消失」的机制。
  queue->Quit();
  runner.join();
  bool after_quit = queue->PostTask(FROM_HERE, base::BindOnce([]() {}));
  LOG(INFO) << "(b) Quit() 后 IsQuitting() = " << queue->IsQuitting()
            << "，PostTask 返回 " << after_quit
            << "（false = 目标序列已消失）";
}

// ===========================================================================
// Part 7 — SequencedTaskRunner 默认序列 runner
// ===========================================================================
static void Part7_DefaultRunner() {
  LOG(INFO) << "===== Part 7: SequencedTaskRunner 默认序列 runner =====";

  // (a) 初始没有默认 runner。
  LOG(INFO) << "(a) 初始 HasCurrentDefault() = "
            << base::SequencedTaskRunner::HasCurrentDefault();

  auto queue = base::MakeRefCounted<base::TaskQueue>();
  // (b) SetCurrentDefault 把某个 runner 设为「当前序列的默认 runner」，由
  //     base::Thread / base::test::TaskEnvironment 自动完成。
  base::scoped_refptr<base::SequencedTaskRunner> def_runner(queue.get());
  base::SequencedTaskRunner::SetCurrentDefault(def_runner);
  LOG(INFO) << "(b) SetCurrentDefault 后 HasCurrentDefault() = "
            << base::SequencedTaskRunner::HasCurrentDefault();

  // (c) GetCurrentDefault 取回它（没有时返回 nullptr）。
  auto got = base::SequencedTaskRunner::GetCurrentDefault();
  LOG(INFO) << "(c) GetCurrentDefault() 非空 = " << (got.get() != nullptr);

  // (d) SingleThreadTaskRunner 只是 SequencedTaskRunner 的类型别名：avbase 在类型
  //     层面不区分「序列」与「线程」，因为这里每个序列在构造上都是单线程的。
  static_assert(
      std::is_same_v<base::SingleThreadTaskRunner, base::SequencedTaskRunner>,
      "SingleThreadTaskRunner must alias SequencedTaskRunner in avbase");
  LOG(INFO) << "(d) SingleThreadTaskRunner 是 SequencedTaskRunner 的类型别名";

  base::SequencedTaskRunner::SetCurrentDefault(nullptr);  // 清理全局状态
}

int main() {
  LOG(INFO) << "##########################################################";
  LOG(INFO) << "#  task_demo —— base/task 全部功能演示";
  LOG(INFO) << "##########################################################";
  Part1_PostAndOrder();
  Part2_Delayed();
  Part3_Diagnostics();
  Part4_ClearAndCycleRule();
  Part5_ReplyWithResult();
  Part6_RealRunLoop();
  Part7_DefaultRunner();
  LOG(INFO) << "##########################################################";
  LOG(INFO) << "#  task_demo 结束";
  LOG(INFO) << "##########################################################";
  return 0;
}

// examples/base/threading/threading_demo.cc
//
// base/threading 全部功能与用法演示。纯 base，不依赖 FFmpeg/SDL/GL，常驻。
//
// base/threading 只提供两样东西：
//   1. Thread          —— 一条专享线程，内部跑一个 TaskQueue（即一条 sequence）
//   2. platform_thread —— 设置 OS 可见的线程名 / 优先级（pthread 层面）
//
// 与 base/task 的关系：Thread 是 TaskQueue 的“线程外壳”。真正的调度逻辑、
// 延迟排序、引用环 RULE 等都在 base/task（见 task_demo.cc）。本 demo 聚焦
// “线程 + 线程名/优先级”这一层。
//
// 所有演示都是真实多线程运行；跨线程同步用 base::WaitableEvent（不 sleep），
// 确定性来自“先等到完成信号再继续”，而非计时。
//
// 手动编译（无需 CMake，链接 base 静态库）：
//   clang++ -std=c++20 -DAVBASE_ENABLE_DCHECK -I. \
//       examples/base/threading/threading_demo.cc build/full/lib/libavbase_base.a \
//       -lpthread -o /tmp/threading_demo
//
// 通过 CMake 构建：
//   cmake --build build/full --target threading_demo

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/sequence_checker.h"
#include "base/synchronization/waitable_event.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread.h"

using namespace avbase;  // 示例而非 SDK：base/ 直接以 base:: 引用

// ======================================================================
// WaitableEvent 总览 —— 本 demo 用它在跨线程场景里“精确等某件事发生”
// ======================================================================
// 本质：基于 std::condition_variable + std::mutex 封装的线程同步原语
//       (waitable_event.h:11-12, 84-87)，即“一个带条件的布尔标志”。
//
// 作用：一个线程喊“某件事发生了”，另一个线程精确地在那件事发生后才被唤醒，
//       既不忙等(spin 烧 CPU)也不瞎 sleep 猜时间。在 avbase 里主要用于
//       “有界关闭等待”——让 ~Player() 永不无限阻塞 (waitable_event.h:18-20,
//       docs/04 §5.4, Δ15)。
//
// 五个方法：
//   Signal()        置“有信号”，notify_all 唤醒所有等待者（必须持锁，见坑①）
//   Reset()         手动清回“无信号”（仅 manual-reset 需要）
//   Wait()          阻塞到信号为止，可能永久阻塞
//   TimedWait(t)    带超时等待，返回 true=收到 / false=超时——有界等待的核心
//   IsSignaled()    非阻塞查当前状态（只快照）
//
// 两种重置策略（构造默认 kManualReset，waitable_event.h:33，刻意对齐
// Chromium）：
//   kManualReset   Signal 后信号一直保持，所有等待者都能过，直到显式 Reset()。
//                 适合“广播一个已经发生、且持续成立的状态”(如退出标志置位)。
//   kAutomaticReset  Signal 唤醒“一个”等待者后自动清回无信号，其余继续睡。
//                 适合“一次只放行一个令牌”。
//   本 demo 全部用 kManualReset 的 done 事件：worker 跑完 Signal()，主线程
//   done.Wait() 确定性地等完成，全程不 sleep。
//
// 为什么比 sleep 好：① 精确唤醒，不浪费也不提前；② 建立 happens-before——
//   Wait() 返回后再读共享变量内存可见性有保证(demo WaitFor 注释)，sleep
//   不提供。
//
// 三个使用约束（来自头文件注释，违反即触发对应问题）：
//   ① Signal() 必须持锁(waitable_event.h:44-47)：先 notify 再解锁会开
//      use-after-free 窗口——等待者从 TimedWait 返回后可能立刻析构本 event，
//      而 notify 还在内部跑，ThreadSanitizer 能抓到。
//   ② 析构安全(waitable_event.h:39-41)：析构时不能有线程还卡在 Signal()/Wait()
//      里。Thread::Stop() 靠先 join 再析构成员来满足。本 demo 的 done 是栈上
//      对象，主线程 Wait() 返回后 worker 不再触碰它，故析构安全。
//   ③ 禁止拷贝(waitable_event.h:37-38)：同步原语不能值语义，只能传引用/指针。
//
// 它和 Thread/TaskQueue 的关系：WaitableEvent 是最底层同步原语（一个标志 +
//   一个条件变量，不保护临界区，不是锁）；Thread 内部自带 TaskQueue 做调度，
//   而“主线程精确等某个外部 worker 干完一件具体的事”用 WaitableEvent 比
//   “往队列投任务再等”更直接。

namespace {

// 一次性等待所有已投递任务跑完的小工具：主线程投递 N 个计数任务 + 1 个哨兵，
// 哨兵 Signal 后主线程再继续，从而保证读取共享变量前 happens-before 成立。
void WaitFor(std::atomic<int>& counter, int expect, base::WaitableEvent& done) {
  done.Wait();
  LOG(INFO) << "   等到完成信号：counter = " << counter.load() << " (期望 "
            << expect << ")";
}

}  // namespace

int main() {
  LOG(INFO) << "===== base/threading 演示 =====";

  // ----------------------------------------------------------------------
  // (a) 基本生命周期：构造 / Start / IsRunning / task_runner() / name() /
  //     GetThreadId() / Stop（幂等）。
  // ----------------------------------------------------------------------
  LOG(INFO) << "(a) 基本生命周期";
  {
    base::Thread worker("avbase-demo");
    LOG(INFO) << "   构造后 IsRunning() = " << worker.IsRunning();
    LOG(INFO) << "   构造后即可拿到 task_runner(): "
              << (worker.task_runner() != nullptr);
    CHECK(worker.Start());
    LOG(INFO) << "   Start 后 IsRunning() = " << worker.IsRunning();
    LOG(INFO) << "   线程名 name() = " << worker.name();
    LOG(INFO) << "   线程 id 有效(非空): "
              << (worker.GetThreadId() != std::thread::id());
    worker.Stop();
    LOG(INFO) << "   Stop 后 IsRunning() = " << worker.IsRunning();
    // Stop 幂等：析构时 ~Thread() 会再调一次 Stop()，已停止则直接返回。
  }

  // ----------------------------------------------------------------------
  // (b) 提前投递：task_runner() 在 Start() 前就有效，Start 前 PostTask 会被
  //     队列缓存，等 Start() 后循环启动才执行。
  // ----------------------------------------------------------------------
  LOG(INFO) << "(b) 提前投递（Start 前 PostTask 被缓存）";
  {
    base::Thread w("avbase-pre");
    std::atomic<int> pre{0};
    // Start 前投递 —— 此时线程还没跑，但 task_runner() 已可用。
    w.task_runner()->PostTask(FROM_HERE,
                              base::BindOnce([&pre] { pre.fetch_add(1); }));
    CHECK(w.Start());  // 循环启动后，缓存的任务才被执行
    base::WaitableEvent done(base::WaitableEvent::ResetPolicy::kManualReset);
    w.task_runner()->PostTask(FROM_HERE,
                              base::BindOnce([&done] { done.Signal(); }));
    WaitFor(pre, 1, done);
    w.Stop();
  }

  // ----------------------------------------------------------------------
  // (c) 顺序与同线程：SequencedTaskRunner 保证任务在同一线程上“按投递序、
  //     一次一个”串行执行。这是 sequence 的本质。
  // ----------------------------------------------------------------------
  LOG(INFO) << "(c) 顺序保证 + 同一线程（serialized sequence）";
  {
    base::Thread w("avbase-seq");
    CHECK(w.Start());
    std::vector<int> order;
    std::vector<std::thread::id> ids(5);
    base::WaitableEvent done(base::WaitableEvent::ResetPolicy::kManualReset);
    for (int i = 0; i < 5; ++i) {
      w.task_runner()->PostTask(FROM_HERE, base::BindOnce([&order, &ids, i] {
                                  order.push_back(i);
                                  ids[i] = std::this_thread::get_id();
                                }));
    }
    w.task_runner()->PostTask(FROM_HERE,
                              base::BindOnce([&done] { done.Signal(); }));
    done.Wait();
    const bool in_order = (order == std::vector<int>{0, 1, 2, 3, 4});
    const bool same_thread =
        std::all_of(ids.begin(), ids.end(),
                    [&](std::thread::id id) { return id == ids[0]; });
    LOG(INFO) << "   执行顺序 = [" << order[0] << "," << order[1] << ","
              << order[2] << "," << order[3] << "," << order[4]
              << "]  严格按投递序: " << in_order;
    LOG(INFO) << "   全部在同一线程执行: " << same_thread;
    LOG(INFO) << "   该线程 id == worker.GetThreadId(): "
              << (ids[0] == w.GetThreadId());
    w.Stop();
  }

  // ----------------------------------------------------------------------
  // (d) 序列判定与诊断 API：
  //     RunsTasksInCurrentSequence() —— 任务内部为 true，外部（主线程）为 false
  //     GetPendingTaskCount() / GetNextReadyTime() —— 队列诊断
  // ----------------------------------------------------------------------
  LOG(INFO) << "(d) 序列判定与诊断 API";
  {
    base::Thread w("avbase-diag");
    CHECK(w.Start());
    std::atomic<bool> inside_seq{false};
    std::atomic<bool> outside_seq{false};
    // 外部视角：主线程不在该 sequence 上
    outside_seq = w.task_runner()->RunsTasksInCurrentSequence();
    w.task_runner()->PostTask(
        FROM_HERE, base::BindOnce([&inside_seq, &w] {
          // 任务内部：当前默认 runner 即本线程的队列，判定为真
          inside_seq = w.task_runner()->RunsTasksInCurrentSequence();
        }));

    // 单独演示诊断：投递一个延迟任务后，pending 立即可见、且 GetNextReadyTime
    // 落在未来（不为 null）。
    std::atomic<size_t> pending{0};
    base::WaitableEvent done(base::WaitableEvent::ResetPolicy::kManualReset);
    w.task_runner()->PostDelayedTask(FROM_HERE,
                                     base::BindOnce([&done] { done.Signal(); }),
                                     base::TimeDelta::FromMilliseconds(50));
    pending = w.task_runner()->GetPendingTaskCount();
    const bool has_future = !w.queue()->GetNextReadyTime().is_null();
    LOG(INFO) << "   外部 RunsTasksInCurrentSequence() = " << outside_seq
              << " (应为 false)";
    LOG(INFO) << "   延迟任务投递后 GetPendingTaskCount() = " << pending
              << "   GetNextReadyTime 落在未来: " << has_future;

    // 等到上面的 immediate 任务跑完再读 inside_seq
    base::WaitableEvent seq_done(
        base::WaitableEvent::ResetPolicy::kManualReset);
    w.task_runner()->PostTask(
        FROM_HERE, base::BindOnce([&seq_done] { seq_done.Signal(); }));
    seq_done.Wait();
    LOG(INFO) << "   内部 RunsTasksInCurrentSequence() = " << inside_seq
              << " (应为 true)";
    // 等延迟哨兵完成，确保队列清空再 Stop
    done.Wait();
    w.Stop();
  }

  // ----------------------------------------------------------------------
  // (e) 真实跨线程投递 + 关闭语义（关键差异点）：
  //     (e1) 优雅关闭：先等到所有任务完成，再 Stop —— 一个都不丢。
  //     (e2) Stop 会丢弃尚未运行的 pending 任务：Run 循环在顶部检查 quit_，
  //          一旦 Quit() 被调用立即 break，未弹出的任务被丢（析构时 clear）。
  //          —— 这与 Chromium Thread::Stop 的“flush 默认 drain”语义不同！
  // ----------------------------------------------------------------------
  LOG(INFO) << "(e) 真实跨线程投递与关闭语义";
  {
    // (e1) 优雅关闭
    base::Thread w("avbase-grace");
    CHECK(w.Start());
    std::atomic<int> count{0};
    base::WaitableEvent all_done(
        base::WaitableEvent::ResetPolicy::kManualReset);
    for (int i = 0; i < 100; ++i) {
      w.task_runner()->PostTask(
          FROM_HERE, base::BindOnce([&count] { count.fetch_add(1); }));
    }
    w.task_runner()->PostTask(
        FROM_HERE, base::BindOnce([&all_done] { all_done.Signal(); }));
    all_done.Wait();
    LOG(INFO) << "   (e1) 100 个跨线程任务全部完成 count = " << count.load();
    w.Stop();  // 此刻队列已空，Stop 不丢任何任务

    // (e2) Stop 丢弃 pending：投递 5 个延迟任务（50ms 后才就绪），主线程
    //      立即 Stop。Run 循环在顶部看到 quit_ 为真即 break，未就绪的任务
    //      一个都不会被弹出执行。
    base::Thread w2("avbase-drop");
    CHECK(w2.Start());
    std::atomic<int> dropped{0};
    for (int i = 0; i < 5; ++i) {
      w2.task_runner()->PostDelayedTask(
          FROM_HERE, base::BindOnce([&dropped] { dropped.fetch_add(1); }),
          base::TimeDelta::FromMilliseconds(50));
    }
    w2.Stop();  // 立即 Quit
    LOG(INFO) << "   (e2) Stop 前未就绪的延迟任务被丢弃: dropped = "
              << dropped.load() << " (应为 0)";
    LOG(INFO) << "   教训：Stop 不等任务跑完。要优雅关闭，须先等到 in-flight "
                 "任务完成，或自身在任务里发起 Quit。";
  }

  // ----------------------------------------------------------------------
  // (f) platform_thread：OS 可见的线程名 / 优先级。
  //     SetName/GetName —— 设置/读取 pthread 名（Linux & macOS 都截断到 15
  //     字）。 SetCurrentThreadPriority —— Linux 走 setpriority 生效；macOS 为
  //     no-op。
  // ----------------------------------------------------------------------
  LOG(INFO) << "(f) platform_thread：OS 可见线程名与优先级";
  {
    // 在主线程上设名并读回（截断到 15 字，这里用 11 字的安全名）
    base::platform_thread::SetName("avbase-main");
    LOG(INFO) << "   SetName(\"avbase-main\") 后 GetName() = "
              << base::platform_thread::GetName();

    // 在 worker 内部读取它被 Thread::ThreadMain 自动设置的 OS 名
    base::Thread w("avbase-named");
    CHECK(w.Start());
    std::string worker_name;
    base::WaitableEvent done(base::WaitableEvent::ResetPolicy::kManualReset);
    w.task_runner()->PostTask(FROM_HERE, base::BindOnce([&worker_name, &done] {
                                worker_name = base::platform_thread::GetName();
                                done.Signal();
                              }));
    done.Wait();
    LOG(INFO) << "   worker 被框架自动命名: GetName() = " << worker_name;
    w.Stop();

    // 优先级：Linux 真实生效，macOS 此处为 no-op（无对应实现分支）
    base::platform_thread::SetCurrentThreadPriority(
        base::ThreadPriority::kBackground);
    LOG(INFO) << "   SetCurrentThreadPriority(kBackground) 已调用"
                 "（Linux 生效，macOS 为 no-op）";
  }

  // ----------------------------------------------------------------------
  // (g) Thread::Options 与 StartWithOptions：自定义 name / priority /
  // stack_size。
  //     avbase 每 Player 恰好创建 6 条线程（docs/04 §2），优先级分三档。
  // ----------------------------------------------------------------------
  LOG(INFO) << "(g) Thread::Options 与三档优先级";
  {
    const base::ThreadPriority levels[] = {
        base::ThreadPriority::kBackground, base::ThreadPriority::kBestEffort,
        base::ThreadPriority::kDisplayCritical,  // 音视频渲染线程用这一档
    };
    for (base::ThreadPriority p : levels) {
      base::Thread t("avbase-prio");
      base::Thread::Options opt;
      opt.name = "avbase-prio";
      opt.priority = p;
      CHECK(t.StartWithOptions(opt));
      LOG(INFO) << "   优先级档位 " << static_cast<int>(p)
                << " 启用成功 IsRunning=" << t.IsRunning();
      t.Stop();
    }
  }

  // ----------------------------------------------------------------------
  // (h) 一个 Thread = 一条 sequence：SEQUENCE_CHECKER 的运行时基础。
  //
  // SequenceChecker 的作用：在“首次”调用时记下当前线程 id，之后每次调用都
  // DCHECK 是否仍是同一线程；不一致即 debug 下断言失败。它不提供任何锁、不
  // 阻止并发，只是开发期把“方法被用错线程/序列”提前炸出来。
  //
  // 为什么用线程 id 当 sequence：avbase 每条逻辑 sequence 都绑定一条专享线程
  // （docs/04 §2），线程身份就是 Chromium sequence 令牌的忠实替身 —— 这正是
  // Thread 的价值所在。
  //
  // 三个宏（DCHECK 关闭时整段塌成空，release 零开销）：
  //   SEQUENCE_CHECKER(name)           声明一个 mutable 成员守卫
  //   DCHECK_CALLED_ON_VALID_SEQUENCE  方法开头断言“只在该序列上被调”
  //   DETACH_FROM_SEQUENCE            换线程前清绑定，允许重新绑定
  //
  // 两个要点：① 一次性绑定、不能迁移 —— 想换线程要先 DetachFromSequence()；
  //          ② 宏带 mutable，只能作类成员，局部场景直接用 SequenceChecker sc。
  // ----------------------------------------------------------------------
  LOG(INFO) << "(h) 一个 Thread = 一条 sequence（SEQUENCE_CHECKER 基础）";
  {
    base::Thread w("avbase-seqbound");
    CHECK(w.Start());
    base::SequenceChecker sc;  // 局部变量；运行时判定与 DCHECK 开关无关
    std::atomic<bool> bound_ok{false};
    std::atomic<bool> rebound_ok{false};
    base::WaitableEvent done(base::WaitableEvent::ResetPolicy::kManualReset);
    // 第一次在该 sequence 上校验：绑定到 worker 线程，返回 true
    w.task_runner()->PostTask(FROM_HERE, base::BindOnce([&sc, &bound_ok] {
                                bound_ok = sc.CalledOnValidSequence();
                              }));
    // 第二次：仍是同一线程，返回 true（不会因“重复调用”而失败）
    w.task_runner()->PostTask(FROM_HERE,
                              base::BindOnce([&sc, &rebound_ok, &done] {
                                rebound_ok = sc.CalledOnValidSequence();
                                done.Signal();
                              }));
    done.Wait();
    LOG(INFO) << "   首次 CalledOnValidSequence() = " << bound_ok
              << " (应为 true)";
    LOG(INFO) << "   同线程再次调用 = " << rebound_ok << " (应为 true)";
    // 注意：若把 sc 带到另一条线程再调用，DCHECK 会崩 —— 这正是它拦“跨线程
    // 误用对象”的方式。Thread 保证每个对象只被它自己的 sequence 触碰。
    w.Stop();
  }

  LOG(INFO) << "===== 演示结束 =====";
  return 0;
}

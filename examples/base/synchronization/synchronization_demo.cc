// examples/base/synchronization/synchronization_demo.cc
//
// base/synchronization 全部功能与用法演示。
// 纯 base，不依赖 FFmpeg/SDL/GL，常驻。
//
// 目录共 7 个文件：WaitableEvent 已在 threading_demo 演示（它是跨线程“就绪
// 信号”，见该 demo 顶部总览），本 demo 覆盖其余 6 个：
//   lock.h                       —— Lock / AutoLock / AutoUnlock
//   lock.cc                      —— 死锁顺序检查器（本 demo (b) 段逐方法讲解）
//   condition_variable.h         —— ConditionVariable（配 Lock）
//   atomic_flag.h                —— AtomicFlag（跨线程取消/中断标志）
//   atomic_sequence_number.h     —— AtomicSequenceNumber（序列号/代际号）
//   waitable_event.h             —— 见 threading_demo（本 demo 仅复用其做同步）
//
// 与 base/threading 的关系：threading 是“线程外壳”，synchronization 是“线程
// 之间如何正确地共享状态”——锁、条件变量、原子标志、原子计数器。其中 Lock 还
// 内置死锁顺序检查（lock.cc 的 LockOrderChecker），正是 avbase “所有锁必须
// 遵循 docs/04 §3.1 单一全局顺序”这一硬规矩在 debug 构建里的执行点。
//
// 手动编译（无需 CMake，链接 base 静态库）：
//   clang++ -std=c++20 -DAVBASE_ENABLE_DCHECK -I. \
//       examples/base/synchronization/synchronization_demo.cc \
//       build/full/lib/libavbase_base.a -lpthread -o /tmp/sync_demo
//
// 通过 CMake 构建：
//   cmake --build build/full --target synchronization_demo

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

#include "base/logging.h"
#include "base/synchronization/atomic_flag.h"
#include "base/synchronization/atomic_sequence_number.h"
#include "base/synchronization/condition_variable.h"
#include "base/synchronization/lock.h"
#include "base/synchronization/waitable_event.h"

using namespace avbase;  // 示例而非 SDK：base/ 直接以 base:: 引用

namespace {

// ----------------------------------------------------------------------
// (a) Lock / AutoLock / AutoUnlock —— 最基本的互斥与临界区保护
// ----------------------------------------------------------------------
// Lock 是 std::mutex 的薄封装（lock.h:57）。封装只为三件事：可被
// tools/check_invariants.py 审计命名、可在 debug 构建断言全局锁顺序
// （docs/04 §3.1）、给 clang 的 GUARDED_BY 注解一个具体类型。
//
// AutoLock（lock.h:82）是 RAII：构造即 Acquire、析构即 Release。它构造时还会
// 调 lock_.AssertAcquiredInOrder()（lock.h:85），即自动接入锁顺序检查器。
// AutoUnlock（lock.h:99）相反：构造释放、析构重新获取——用于“持锁期间要临时
// 让出锁去干别的事”的场景。

void DemoLock() {
  base::Lock lock;
  std::atomic<int> counter{0};

  // 8 个线程各自自增 1000 次，靠 AutoLock 保护临界区，结果应为 8000（无竞态）。
  std::vector<std::thread> ts;
  for (int i = 0; i < 8; ++i) {
    ts.emplace_back([&lock, &counter] {
      for (int k = 0; k < 1000; ++k) {
        base::AutoLock autolock(lock);  // 构造 Acquire + 顺序检查
        counter.fetch_add(1);           // 临界区
      }                                 // 析构 Release + RecordRelease
    });
  }
  for (auto& t : ts) t.join();

  LOG(INFO) << "  (a) 8×1000 自增受 AutoLock 保护，counter = " << counter.load()
            << " (应为 8000，无竞态)";

  // AutoUnlock 演示：持锁 → 临时释放 → 重新获取。用来“持锁状态下需等待别的
  // 线程、又不希望一直占着锁”时用。
  {
    base::AutoLock outer(lock);  // 持锁
    {
      base::AutoUnlock temp(lock);  // 构造 Release（让出锁）
      // 此处锁已临时释放，其它线程可进入临界区
    }  // 析构时 Acquire 重新拿回
    // 回到持锁状态，可继续安全访问临界区
  }
  LOG(INFO) << "  (a) AutoUnlock 临时让出并重新获取锁：正常结束";
}

// ----------------------------------------------------------------------
// (b) Lock 顺序检查 —— 逐方法解释 base/synchronization/lock.cc
// ----------------------------------------------------------------------
// lock.cc 在 AVBASE_ENABLE_DCHECK 开启时定义了四个东西（关闭时两个成员方法
// 退化成空实现，见 lock.cc:83-84，零开销但不抓反转）：
//
//  1) HeldStack() —— 每个线程一个 thread_local<std::vector<const Lock*>>，
//     记录“本线程当前正持有（且未释放）的锁栈”。AutoLock 构造把它 push
//     （间接经 AssertAcquiredInOrder），析构由 RecordRelease 把它 erase。
//
//  2) ObservedOrder() —— 每个线程一个 thread_local<
//     vector<pair<const void*, const void*>>>，记录“本线程观察过的加锁顺序
//     对 (A,B) 表示 A 在 B 之前获取”。这是死锁检测的记忆体。
//
//  3) Lock::AssertAcquiredInOrder() —— 死锁顺序检查的核心。每拿到一把新锁
//     this，它遍历 HeldStack 里已持有的每把 outer：
//       · 若曾记录过 reverse=(this,outer)（this 在 outer 之前）而现在又要
//         (outer,this) 即 forward，且 forward 尚未记录过 → 说明观察到了锁顺序
//         反转 → LOG(FATAL) "Lock order inversion detected"（lock.cc:60-70）。
//         锁顺序反转会导致死锁，所以必须 FATAL 而非仅报警。
//       · 否则把 forward=(outer,this) 记进 ObservedOrder（若还没记录过）。
//     最后把 this push 进 HeldStack。
//
//  4) Lock::RecordRelease() —— 释放时把 this 从 HeldStack 抹掉（lock.cc:77-79），
//     使“已释放的锁”不再参与后续的顺序对照。AutoLock 析构在 Release() 之后
//     调它（lock.h:90-93）。
//

base::Lock lock_a;  // 全局顺序里 lock_a 必须在 lock_b 之前（docs/04 §3.1）
base::Lock lock_b;

void DemoLockOrder() {
  // 始终以 (lock_a, lock_b) 顺序持锁：顺序检查器记录 forward=(a,b) 且永不
  // 看到 reverse，因此不会触发 FATAL——正常运行。
  {
    base::AutoLock a(lock_a);  // 检查器：held=[a]
    base::AutoLock b(lock_b);  // 检查器：记 (a,b)，held=[a,b]
    // 临界区：同时持有 a、b
  }  // b 析构：Release + RecordRelease 把 b 从 held 抹掉；a 析构同理

  LOG(INFO) << "  (b) 以 (a,b) 固定顺序持锁，顺序检查器记录 (a,b) 未反转：正常";
  LOG(INFO) << "  (b) 若某线程以 (b,a) 顺序持锁，将触发 LOG(FATAL) 锁顺序反转";
}

// ----------------------------------------------------------------------
// (c) ConditionVariable —— 配 Lock 的生产者/消费者等待
// ----------------------------------------------------------------------
// ConditionVariable（condition_variable.h:18）必须绑一把 base::Lock（不是裸
// std::mutex），这样锁顺序检查器仍能看见它。Wait() 要求调用者已持有锁，它
// 在等待期间 adopt 这把锁并释放、被唤醒后重新持有（condition_variable.h:28-32）。
// 这正好解决了“持锁状态下要等条件、又不想一直占着锁”的矛盾。

void DemoConditionVariable() {
  base::Lock lock;
  base::ConditionVariable cv(&lock);
  int produced = 0;
  int consumed = 0;
  constexpr int kTotal = 5;

  // 消费者：kTotal 个线程各等一次广播后消费一次
  std::vector<std::thread> consumers;
  for (int i = 0; i < kTotal; ++i) {
    consumers.emplace_back([&] {
      base::AutoLock l(lock);
      while (consumed >= produced)  // 没东西可吃就等（兼防虚假唤醒）
        cv.Wait();                  // 持有 lock → 释放等待 → 唤醒重持
      ++consumed;
    });
  }

  // 生产者：主线程持锁生产 kTotal 个后 Broadcast 唤醒所有消费者
  {
    base::AutoLock l(lock);
    for (int i = 0; i < kTotal; ++i) ++produced;
    cv.Broadcast();  // 唤醒所有等待的消费者（notify_all）
  }

  for (auto& t : consumers) t.join();
  LOG(INFO) << "  (c) 生产者产出 " << produced << "，消费者共消费 " << consumed
            << " (应为 " << kTotal << "，等待期间锁被正确释放/重持)";
}

// ----------------------------------------------------------------------
// (d) AtomicFlag —— 跨线程取消 / 中断标志
// ----------------------------------------------------------------------
// AtomicFlag（atomic_flag.h:22）是跨线程的“一次性布尔旗”。avbase 每 Player
// 用三个，分别覆盖“停止 / 暂停 / 缓冲结束”等取消点。与 Chromium 不同，avbase
// 额外提供 Reset()：因为 demuxer 的“中断标志”必须在每次 seek 前清掉——它被用来
// 打断阻塞的 av_read_frame，用完要清掉让下次读取能继续；停止标志则实践中始终
// 单向、从不 Reset。

void DemoAtomicFlag() {
  base::AtomicFlag stop;
  base::WaitableEvent started(base::WaitableEvent::ResetPolicy::kManualReset);
  base::WaitableEvent done(base::WaitableEvent::ResetPolicy::kManualReset);
  std::atomic<int> work{0};

  std::thread worker([&] {
    started.Signal();  // 先就位，让主线程确认已进入循环
    // 可被中断的工作循环：每次迭代检查取消标志
    while (!stop.IsSet()) {
      work.fetch_add(1);
    }
    done.Signal();  // 通知主线程已退出
  });

  started.Wait();   // 等 worker 进入循环（避免主线程抢先 Set 导致 work=0）
  stop.Set();       // 取消 worker
  done.Wait();      // 精确等 worker 真正退出（而非 sleep 猜）
  worker.join();

  LOG(INFO) << "  (d) AtomicFlag 取消生效：worker 在 IsSet() 后退出，work = "
            << work.load();

  // Reset 演示：seek 前清掉中断标志，使后续读取能继续
  base::AtomicFlag interrupt;
  interrupt.Set();                 // 上次 seek 设置的中断
  interrupt.Reset();               // seek 前清除
  LOG(INFO) << "  (d) Reset() 后 IsSet() = " << interrupt.IsSet()
            << " (应为 0，下一次读取可继续)";
}

// ----------------------------------------------------------------------
// (e) AtomicSequenceNumber —— 单调递增序列号 / 代际号
// ----------------------------------------------------------------------
// AtomicSequenceNumber（atomic_sequence_number.h:13）用于 seek 代际号(serial)
// 和请求 id。GetNext() 用 acq_rel 原子自增并返回新值，多线程并发调用也保证
// 全局唯一、无重复；Get() 取当前值（不递增）。

void DemoAtomicSequenceNumber() {
  base::AtomicSequenceNumber serial;
  std::vector<int> ids;
  base::Lock ids_lock;

  // 模拟 4 个线程各自取若干 request id
  std::vector<std::thread> ts;
  for (int i = 0; i < 4; ++i) {
    ts.emplace_back([&] {
      for (int k = 0; k < 100; ++k) {
        int id = serial.GetNext();  // 原子递增，全局唯一
        base::AutoLock l(ids_lock);
        ids.push_back(id);
      }
    });
  }
  for (auto& t : ts) t.join();

  // 验证：400 个 id 排序后应恰好是 1..400 无重复
  std::sort(ids.begin(), ids.end());
  bool unique = true;
  for (size_t i = 0; i < ids.size(); ++i)
    if (ids[i] != static_cast<int>(i) + 1) { unique = false; break; }

  LOG(INFO) << "  (e) 400 个并发 GetNext() 结果唯一且连续 = "
            << (unique ? "true" : "false")
            << "，当前 Get() = " << serial.Get();
}

}  // namespace

int main() {
  LOG(INFO) << "===== base/synchronization 演示 =====";
  DemoLock();
  DemoLockOrder();
  DemoConditionVariable();
  DemoAtomicFlag();
  DemoAtomicSequenceNumber();
  LOG(INFO) << "===== 演示结束 =====";
  return 0;
}

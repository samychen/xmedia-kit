# `base/` 入门学习指南

> **面向**：刚接手 avbase、需要读懂并安全改动 `base/` 的人。
> **基线**：commit `2deca4a`（工作区干净，故 HEAD == 工作区）。
> **方法**：双轴审查 —— 轴一看代码是否遵守本仓库明文标准，轴二看代码是否
> 兑现设计文档的清单；两条结果**分开呈现、不合并排序**，最后逐条复核。
> **状态**：未提交。

---

## 0. 怎么用这份文档

`base/` 共 **45 个受版本控制的文件**（33 个 `.h` + 11 个 `.cc` + `CMakeLists.txt`），
**4069 行代码**。它的特殊之处是：**它不是业务代码，而是整个项目的语法**。
`media/`、`player/`、`platform/` 都用它说话，所以这里的一个设计取舍会被放大到全树。

按下面三条纪律读，比从上往下读快得多：

1. **按依赖顺序读，不按字母序。** 先 `check.h` → `time.h` → `memory/` →
   `functional/` → `task/` → `threading/`。后面的每一站都用前面站的东西。
2. **每读一个文件先读它的头注释。** 这个目录的头注释密度是刻意的，多数文件
   前 10 行就回答了"为什么这样设计"，而这类信息在 Chromium 里是散落的。
3. **读一个文件，同时读它在 `tests/unit/base/` 的测试。** 11 个测试文件共
   2115 行，是意图最清楚的说明书。

| 目录 | 行数 | 一句话 |
|---|---|---|
| `base/functional/` | 311+188+68+20 | `BindOnce` 子集 + `OnceCallback`/`RepeatingCallback` |
| `base/time/` | 309+61+47+28+26+14 | `TimeDelta`/`TimeTicks` + 可注入时钟 |
| `base/memory/` | 249+137+182+95+31 | 三个所有权动词（`MakeRefCounted`/`WrapRefCounted`/`AdoptRef`）|
| `base/task/` | 173+119+65+62+42+41+29 | **avbase 的消息循环**（`TaskQueue`）|
| `base/synchronization/` | 112+92+89+53+39+31 | `Lock`/`ConditionVariable`/`WaitableEvent` + 三个原子件 |
| `base/threading/` | 81+74+69+34 | `base::Thread` + 平台线程封装 |
| `base/test/` | 103+79 | `TaskEnvironment`（单测地基）|

---

## 1. 第一件事：搞清楚 `base/` 为什么存在

在 `docs/02-总体架构与模块划分.md` §3.1（第 104 行）有一张三选一的表，先读它：

| 方案 | 结论 |
|---|---|
| A. 从 Chromium 抽 `base/` 子树 | ❌ 要拖 absl + BUILD.gn + 30k 用不到的行 |
| B. 用 Abseil | ❌ Abseil 明确不提供 task runner / `BindOnce` / `WeakPtr` / `TimeTicks` |
| C. **自研同名同语义最小子集** | ✅ 调用方形态与 Chromium 完全一致，将来换 vendored 只换实现 |

**这张表是理解整个目录的钥匙**：`base/` 的每一个文件都是「照 Chromium 的**接口**
重新实现」，不是照它的实现抄。所以你会看到很多文件的头注释写着
`API mirrors Chromium's base/xxx.h (BSD-3-Clause)` —— 33 个头文件里有 **25 个**
带这行。缺的 8 个见 §5。

配套的「降级预案」在 `docs/08` §5.1 的 **R2**（第 195 行）：如果 `bind.h` /
`message_pump_epoll` / `TaskEnvironment` 三个大件工期超支，就分 L1/L2 降级，
但**接口形态保持不变**，这样 `media/` 和 `player/` 零改动。

> ⚠️ **文档自己的交叉引用漂了**：`docs/02` 第 145 行写「见 [08 §5 R16]」，但
> `docs/08` 的 **R16 是 Wayland 嵌入**（第 166 行），base 的风险条目是 **R2**。
> 想看 base 的降解预案要看 R2，不是 R16。

---

## 2. 阅读路线

### 第 1 站 · 断言与日志 —— `check.h`（94 行）、`logging.{h,cc}`（147+103）

**看什么**：`check.h:19-50`。

```cpp
// CHECK() is active in all build configurations.
#define CHECK(condition) ...
#define DCHECK(condition) CHECK(condition)   // 仅 AVBASE_ENABLE_DCHECK 下
```

**为什么重要**：这是全项目唯一被允许的"失败方式"。项目用 `-fno-exceptions`
编译（STYLE.md §1），没有异常可抛，所以**所有错误要么走返回值要么走 CHECK**。

**初学者常踩的坑**：把 `DCHECK` 当成断言。它只在开了 `AVBASE_ENABLE_DCHECK`
时才生效，而且 `NOTREACHED()` 在 release 下也会终止程序。判断标准很简单：
**条件来自外部输入 → 必须 `CHECK`；条件是内部不变量 → 可以 `DCHECK`**。

配套 `logging.h` 的 `LoggingDelegate` 是 SDK 用户接管日志输出的接缝（默认
stderr）。

**可执行的对照**：`tests/unit/base/check_unittest.cc`（12 个用例）。它把上面
两条坑各钉成断言，其中失败路径用 gtest 的死亡测试（`EXPECT_DEATH`）在子进程
里观察——这是唯一能断言"它会 abort"的办法。文件头的注释列了三个容易搞错的
点，值得先读注释再读用例。

---

### 第 2 站 · 时间 —— `time.h`（309 行）、`tick_clock.h`、`default_tick_clock.*`、`simple_test_tick_clock.h`

**看什么**：`time.h:42` 附近的 `is_infinite()`，以及 `time.h:178-201` 的
`SaturatingMul`。

`SaturatingMul` 是好例子：**先做除法边界检查，再算乘积**，注释里点明旧代码
先算 `a*b` 是 UB。而且注释写着"溢出证明只放一处" —— 不变量的证明集中，
不在每个调用点各写一遍分支。

---

### 第 3 站 · 所有权 —— `memory/`（五个文件）

这是全项目最该读对的一站。三个动词，**选错一个是生命周期 bug，不是风格问题**：

| 动词 | 语义 | 位置 |
|---|---|---|
| `MakeRefCounted<T>(...)` | **创造**（计数从 0 起，+1） | `scoped_refptr.h` 底部 |
| `WrapRefCounted(p)` | **共享**（+1） | 同上 |
| `AdoptRef(p)` | **接管**（不 +1） | 同上 |

**必读**：`scoped_refptr.h:168-185` 的 `MakeRefCounted` 注释 —— 它解释了为什么
`new T(...)` 是 `base/` 里唯一跨 TU 构造引用计数对象的地方，以及为什么
`MediaLog` 要把构造函数设为 public 而析构函数 private 配 `friend`。这是一段
"注释在讲为什么这么选"的范本，而不是复述代码做了什么。

**必读**：`weak_ptr.h:126-132` 的 `CheckValid()`：

```cpp
void CheckValid() const {
  CHECK(MaybeValid()) << "WeakPtr used after its target was destroyed";
#if defined(AVBASE_ENABLE_DCHECK)
  DCHECK(flag_->CheckSequenceAffinity()) << "WeakPtr dereferenced on ...";
#endif
}
```

**崩溃条件用 `CHECK`，不变量用 `DCHECK`，分层写。** 别混成一个 `DCHECK`。

配套 `raw_ptr.h` 首期只是 `T*` 的注解别名（预留接 BackupRefPtr 的位置）——
它的价值不在运行时，而在**让类型系统替你声明"我不拥有"**。

### 3.1 代码与 test demo

下面这个完整程序把本站的三个动词、`WeakPtr`、`raw_ptr` 全部跑一遍，题目是
「证明引用计数随拷贝/包装/释放正确增减，且对象在最后一个引用释放时析构」。把它和
`build/full` 的 `libavbase_base.a` 一起编（`-DAVBASE_ENABLE_DCHECK` 要带上，
否则 `AddRef` 在旧代码里是空操作 —— 见下面的 ⚠️）：

```sh
clang++ -std=c++20 -DAVBASE_ENABLE_DCHECK -I<repo> demo.cc \
  <repo>/build/full/lib/libavbase_base.a -lpthread -o demo
```

```cpp
#include <cstdio>
#include "base/check.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"

namespace base = avbase::base;

class Foo : public base::RefCountedThreadSafe<Foo> {
 public:
  Foo() { printf("  [Foo] 构造\n"); }
  ~Foo() { printf("  [Foo] 析构（最后一个引用释放）\n"); }
  int use_count() const { return ref_count(); }  // demo 用：暴露内部计数
 private:
  friend class base::RefCountedThreadSafe<Foo>;  // 让 Release() 能碰到析构
};

struct Owner {                 // WeakPtrFactory 必须是最后一个成员
  int value = 42;
  base::WeakPtrFactory<Owner> factory_{this};
};

void demo_station3() {
  printf("\n=== 第 3 站：所有权 ===\n");
  auto a = base::MakeRefCounted<Foo>();              // 创造：计数 0 -> 1
  printf("  a->use_count() = %d\n", a->use_count());

  base::scoped_refptr<Foo> b = a;                   // 共享：+1 -> 2
  printf("  拷贝后 = %d\n", a->use_count());
  auto c = base::WrapRefCounted(a.get());           // 再包一层：+1 -> 3
  printf("  包装后 = %d\n", a->use_count());

  b = nullptr; c.reset();                           // 各 -1 -> 1
  printf("  释放后 = %d\n", a->use_count());

  // AdoptRef 接管「已存在的引用」，不再 +1：a2 让出它持有的那一个引用但不 Release
  auto a2 = base::MakeRefCounted<Foo>();
  Foo* raw = a2.get();
  a2.release();
  auto adopted = base::AdoptRef(raw);               // 接管，计数仍 1（不是 2）
  printf("  adopted->use_count() = %d\n", adopted->use_count());
  adopted.reset();                                  // -> 0 -> 析构

  Owner owner;
  base::WeakPtr<Owner> wp = owner.factory_.GetWeakPtr();
  CHECK(wp);  printf("  wp->value = %d\n", wp->value);
  owner.factory_.InvalidateWeakPtrs();
  CHECK(!wp);   // 失效后变 null，不再是 use-after-free

  base::raw_ptr<Foo> rp = a.get();  // 仅注解「我不拥有」，运行时等同 T*
  printf("  rp->use_count() = %d\n", rp->use_count());
}  // a 离开作用域 -> 最后一个引用释放 -> Foo 析构
```

实测输出（编译时带 `-DAVBASE_ENABLE_DCHECK`）：

```
=== 第 3 站：所有权 ===
  [Foo] 构造
    a->use_count() = 1
  拷贝后 = 2
  包装后 = 3
  释放后 = 1
  [Foo] 构造
    adopted->use_count() = 1
  [Foo] 析构（最后一个引用释放）     <- adopted 释放触发
    wp->value = 42
  rp->use_count() = 1
  [Foo] 析构（最后一个引用释放）     <- a 离开作用域触发
```

> ⚠️ **一个藏在 `AddRef` 里的真 bug（2026-10-24 发现并修复）**：旧写法是
> `DCHECK_GE(ref_count_.fetch_add(1, relaxed), 0)` —— **增量被当成 DCHECK 的参数**。
> 一旦 `AVBASE_ENABLE_DCHECK` 关闭（release 构建 / `release` preset），`DCHECK_GE(a,b)`
> 展开成 `DCHECK(true)`，连 `fetch_add` 一起被编译掉，`AddRef` 变成空操作：计数永远
> 停在 0，第一个 `Release` 把 0 减成 -1 且永不删除 —— **所有 `RefCounted` 对象在
> release 里永不析构、全部泄漏**。修复后增量是独立语句，DCHECK 只做断言：
> `ref_count_.fetch_add(1, relaxed); DCHECK_GE(ref_count_.load(relaxed), 1);`
> 这正是第 8 站「DCHECK 关闭时 `<<` 右侧仍求值，但 `DCHECK(a)` 的参数整段被抹掉」的
> 同一类坑——**任何有副作用的表达式都绝不能藏在断言参数里**。

---

### 第 4 站 · 回调与绑定 —— `functional/`（四个文件）

**规格怎么说的**：`docs/02` 第 119 行标注 `bind.h` 是「★最难的一个，约 800 行；
可先支持子集」。

**实际什么样**：311 行。差的那 489 行不是没做完，是**走了降级预案** —— 证据在
文件头第 7 行：

> `Supported subset (the L1 tier of docs/08 §5.1 R2's fallback plan)`

第 8-17 行列了边界：支持 lambda/functor（非泛型）、自由函数、成员函数（接收者
可以是 `T*`/`const T*`/`scoped_refptr<T>`/`WeakPtr<T>`）、0..N 个绑定参数、
`Unretained()`/`DoNothing()`；**不支持** `Owned()`（因为没人负责 delete，发出去
会静默泄漏）、`Passed()`、`IgnoreResult()` 作为 binder、泛型 lambda。

**这是全目录最值得学的一处**：偏差不是靠"做完"来合法化，而是靠"**写下来**"。
文件头把取舍的边界写死，后来的人不用猜。

### 4.1 代码与 test demo

同一份 demo 的「第 4 站」部分（完整程序见 §3.1）演示了 `BindOnce`/`BindRepeating`、
成员函数 + `WeakPtr` 接收者、目标死亡后的 no-op、`Unretained`、`DoNothing`：

```cpp
struct Worker {
  void Do(int x) { last = x; printf("  [Worker::Do] 收到 %d\n", x); }
  int last = 0;
  base::WeakPtrFactory<Worker> factory_{this};
};

void demo_station4() {
  printf("\n=== 第 4 站：回调与绑定 ===\n");
  int result = 0;
  base::OnceCallback<void()> cb = base::BindOnce([](int* out) { *out = 7; }, &result);
  std::move(cb).Run();  CHECK_EQ(result, 7);          // [1] BindOnce 消费一次

  base::OnceCallback<int(int)> add2 =
      base::BindOnce([](int a, int b) { return a + b; }, 2);
  printf("  add2.Run(3) = %d\n", std::move(add2).Run(3));   // [2] 偏应用

  base::RepeatingCallback<void()> hi = base::BindRepeating([] { printf("  hi~\n"); });
  hi.Run(); hi.Run();                                // [3] 可多次调用

  {                                                  // [4] 成员函数 + WeakPtr 接收者
    Worker w;
    std::move(base::BindOnce(&Worker::Do, w.factory_.GetWeakPtr(), 9)).Run();
    CHECK_EQ(w.last, 9);
  }                                                  // w 在此析构，其工厂先失效

  base::WeakPtr<Worker> wp;                          // [5] 关键安全：目标已死 -> no-op
  {
    Worker w;
    wp = w.factory_.GetWeakPtr();
    std::move(base::BindOnce(&Worker::Do, wp, 5)).Run();
  }                                                  // w 析构 -> wp 失效
  std::move(base::BindOnce(&Worker::Do, wp, 99)).Run();  // 静默 no-op，不崩

  Worker w2;                                         // [6] Unretained：调用方保证生命周期
  std::move(base::BindOnce(&Worker::Do, base::Unretained(&w2), 11)).Run();
  CHECK_EQ(w2.last, 11);

  std::move(base::DoNothing()).Run();                // [7] 可安全 Run 的空闭包
}
```

实测输出（重点看 [5]）：

```
=== 第 4 站：回调与绑定 ===
    result = 7
    add2.Run(3) = 5
  hi~
  hi~
  [Worker::Do] 收到 9
  [Worker::Do] 收到 5
    （若当时绑的是裸指针而非 WeakPtr，这一步就是 use-after-free）
  [Worker::Do] 收到 11
    DoNothing().Run() 安全返回
```

> **[5] 是整段最有价值的观察**：跨序列回调只要绑的是 `WeakPtr`，目标先死 →
> `BindState::operator()` 在 `AnyWeakPtrInvalid()` 处直接提前返回（void 情形静默
> no-op，非 void 情形返回 `R{}` 并靠 `static_assert` 保证 `R` 可默认构造），从结构上
> 消灭了 ijkplayer 无法防范的「player 已释放但回调还在飞」这一类崩溃。绑 `WeakPtr`
> 时**务必传 `WeakPtr` 本身，不要传 `weak_ptr.get()`**——`BindOnce` 只对类型真的是
> `WeakPtr` 的绑定参数跑存活检查，`get()` 出来的裸指针编译得过、看起来一样，却悄悄
> 关掉了守卫。

---

### 第 5 站 · 任务与线程 —— `task/` + `threading/`

`base/task/` 是 avbase 的消息循环，`base/threading/` 是它外面的「线程外壳」：后者只是把前者塞进一条专享线程，再补上 OS 线程名/优先级。先读 `task/`，再读 `threading/`。

#### `task/`：四层，从抽象到落地

| 文件 | 角色 |
|---|---|
| `task_runner.h` (+`.cc`) | `TaskRunner` 抽象基类：`PostTask` / `PostDelayedTask` / `RunsTasksInCurrentSequence` / `GetPendingTaskCount`。调用方只认这一层。 |
| `sequenced_task_runner.h` (+`.cc`) | `SequencedTaskRunner : TaskRunner`——「序列」：任务**一次一个、按投递序**执行；还管默认 runner（`Has`/`Get`/`SetCurrentDefault`）。`SingleThreadTaskRunner` 只是它的 `using` 别名（avbase 不在类型层区分两者，`sequenced_task_runner.h:37`）。 |
| `task_queue.h` (+`.cc`) | `TaskQueue final : SequencedTaskRunner`——唯一的实现：一个有序队列 + 一个 run loop。 |
| `task_runner_util.h` | `PostTaskAndReplyWithResult`：把任务结果回传给 reply。**注意它的注释与实现自相矛盾**（见 §4 T2）。 |

**怎么读 `task_queue.h:24-41`**（spec→code 映射最紧的两处之一）：

```
// This is avbase's message loop. Chromium splits the same responsibility across
// base::MessageLoop, base::MessagePump and base::TaskQueue; avbase collapses
// them because the core needs neither fd watching nor nested RunLoops -- the
// platform layer runs its own poll loop for Wayland/epoll (docs/04 §2.1, D3).
```

也就是说：**规格表里的 `message_loop/` 三项（约 1200 行）不是「漏做」，是被
`TaskQueue`（头+实现 292 行）+ `TaskRunner`/`SequencedTaskRunner` 两层抽象取代了**。注释说明了将来什么条件下要把 `MessagePump` 拆回来：base/ 里一旦需要 fd 监听，就把它拆回原接口，调用方不变。这个决定登记在 `docs/08` §5.1 R2，代价（无 fd/定时器驱动、无嵌套 RunLoop）写在 `docs/archive/PROGRESS-rounds-1-9.md:1295`。

**调用方要记住的三件事：**

1. **顺序由 `(ready_time, insertion_sequence)` 决定**：零延迟任务永远不会超到一条「已到期」的延迟任务前面。这正是 `TaskEnvironment::FastForwardBy()`（第 8 站）能确定性推进的基础——所以第 8 站说「排序是 TaskQueue 保证的（见第 5 站）」，就指这条。
2. **诊断对外可见**：`GetPendingTaskCount()` 看队列长度，`GetNextReadyTime()` 看最早就绪时刻（空则返回 null `TimeTicks`）。测试桩 `RunNextReadyTask` / `RunAllReadyTasks` / `SetTickClockForTesting` / `BindToCurrentSequence` 让 `TaskEnvironment` 能在线程上内联驱动。
3. **引用环 RULE（最硬的一条，见 `task_queue.h:37-41`）**：

```
// RULE: a task must never hold a scoped_refptr to the TaskQueue it is posted
// to. The queue owns the task, so that is a reference cycle and the queue will
// never be destroyed — LeakSanitizer reports it and it is a real leak, not a
// false positive. Capture a base::WeakPtr instead, or call Clear() before
// dropping your own reference.
```

即：**投递进 TaskQueue 的任务，绝不能持有该队列的 `scoped_refptr`**——队列拥有任务、任务又引用队列 = 引用环，队列永远析构不掉，LeakSanitizer 会报（真泄漏，非误报）。要拿队列引用就用 `WeakPtr`，或者丢引用前先 `Clear()`。

**再读 `task_queue.cc:106-117` 的 `Quit()`——并发注释的范本：**

```
// The flag must be set *while holding the lock*. Signalling without it is a
// lost wakeup: Run() can observe quit_==false, then block in cv_.Wait() after
// this broadcast has already been delivered, and never wake again. That hang
// is exactly ijkplayer's "release() blocks forever" class of bug (docs/01
// 病灶 11) ...
```

**并发注释要写出失效时的具体症状**，不写「需要加锁」。⚠️ 顺带一个实测事实：`Run()` 在循环顶部检查 `quit_`，一旦 `Quit()` 被调用立即 `break`，**尚未弹出的 pending 任务会被丢弃**（队列析构时 `clear`）。这与 Chromium `Thread::Stop` 默认的「drain」语义不同——所以关闭线程前要先等 in-flight 任务跑完，或让任务自己发起 `Quit()`（见下面的 `threading/`）。

#### `threading/`：把 TaskQueue 塞进一条专享线程

| 文件 | 角色 |
|---|---|
| `thread.h` (+`.cc`) | `Thread`：一条专享线程，内部跑一个 `TaskQueue`（= 一条 sequence）。`Options{name, priority, stack_size}`。 |
| `platform_thread.h` (+`_posix.cc`) | OS 层面：设线程名、设优先级（pthread）。 |

**`Thread` 的生命周期**：`task_runner()` 在构造后、Start 前就有效（提前投递被缓存，Start 后才跑）；`Start`/`StartWithOptions` 拉起线程；`Stop()` **有界且幂等**——它先 `Quit()` 再 `join`，而 `Quit()` 让 `PostTask` 返回 `false`，于是不会再有任务投进一条正在消失的线程，这正是 `~Player()` 不会被「塞满队列里阻塞的生产者」挂死的原因（`docs/04` §5.4，Δ15）。`~Thread()` 会再调一次 `Stop()`。

**avbase 怎么用**：每个 `Player` **恰好创建 6 条** `Thread`（`docs/04` §2）：`avbase-media` / `avbase-demux` / `avbase-video` / `avbase-audio` / `avbase-text` / `avbase-event`。每条线程独占一组「只有它能碰」的对象——这正是 `SEQUENCE_CHECKER`（第 7 站）运行时校验的基础：**一个 `Thread` = 一条 sequence**，线程身份就是 sequence 令牌的替身（`sequence_checker.h:17-19`）。

**`platform_thread` 的平台差异**：
- `SetName` / `GetName`：走 pthread，Linux 与 macOS **都截断到 15 字**（Linux `comm` 上限；Apple 允许 64 但为统一日志宽度也取 15），所以 avbase 的线程名都短（`avbase-media`…）。
- `SetCurrentThreadPriority`：Linux 走 `setpriority`（kBackground=+10 / kBestEffort=0 / kDisplayCritical=-6，负值需要 `CAP_SYS_NICE`，失败 benign）；**macOS 是 no-op**（无对应实现分支）。

---

### 第 6 站 · 同步原语 —— `synchronization/`

`Lock`/`AutoLock`/`AutoUnlock` 是 `std::mutex` 封装 + 注解；`ConditionVariable`
和 `WaitableEvent` 是它上面的两层。三个原子件（`atomic_flag`、
`atomic_sequence_number`、`condition_variable`）都很短，可以一口气读完。

注意：`WaitableEvent::WaitMany` **没有实现**（规格第 125 行列了它），而
`docs/03` 第 1664 行的关闭流程在设计上依赖它 —— 这是「规格要求了但只部分
落地」里最实质的一条。

---

### 第 7 站 · 其余 —— `observer_list.h`、`location.h`、`sequence_checker.h`、`types/expected.h`、`expected_macros.h`

- `observer_list.h`：实现观察者模式（发布-订阅）的容器——一份**不拥有**的观察者指针清单。核心是在遍历通知的过程中**允许订阅者增删自己或别人**而底层 `vector` 不失效：移除的观察者本轮不再被通知、新增的会被通知到，删除延迟到最外层 `Iterator` 析构才压实（`Entry.alive` 标志 + `iteration_depth_` + `Compact()`，`observer_list.h:42-119`）。`size()`/`HasObserver()` 只数 `alive`，所以遍历中逻辑已移除的观察者不会让 `AddObserver` 的 `CHECK(!HasObserver())` 误报。`Clear()`/`AddObserver`/`RemoveObserver`/`HasObserver`/`empty`/`size`/`Iterator::GetNext` 齐全。**线程不安全（设计如此）**：要么调用处用 `Lock` 守，要么限定单序列。

  **当前落地状态**：`base::ObserverList<T>` 目前**只被它自己的单测 `tests/unit/base/observer_list_unittest.cc` 实例化**，生产代码尚未采用——播放器的观察者注册用的是另一套：`player::EventHub` 自己实现了基于 `std::vector<ObserverEntry>`（int id + `raw_ptr<PlayerObserver>`，`event_hub.h:93-105`）的、带单调递增 id 的注册表，而非本类。所以下面这段是**教学示意**，演示 API 的正确姿势，不代表仓库里存在对应的生产类。

  **典型用法（教学示意——广播时观察者安全增删自身）**：

  ```cpp
  class MediaEventHub {
   public:
    void AddListener(MediaListener* l) { observers_.AddObserver(l); }
    void RemoveListener(MediaListener* l) { observers_.RemoveListener(l); }
    void FirePlaybackStarted() {
      for (base::ObserverList<MediaListener>::Iterator it(&observers_);
           MediaListener* l = it.GetNext();) {
        l->OnPlaybackStarted();  // 这里 l 可安全地 Add/Remove 自己或别人
      }
    }
   private:
    base::ObserverList<MediaListener> observers_;
  };
  ```

- `location.h`：就是 `FROM_HERE`，让每个 `PostTask` 带上源码位置。42 行，五分钟。
- `sequence_checker.h`：`SEQUENCE_CHECKER(x)` / `DCHECK_CALLED_ON_VALID_SEQUENCE(x)` /
  `DETACH_FROM_SEQUENCE(x)` 三宏齐全（规格 ★「并发正确性的基石」）。注意它在
  DCHECK 关闭时**整个消失**（第 52-56 行），所以不要往里面塞有副作用的表达式。

  **作用**：首次调用记下当前线程 id，之后每次都 `DCHECK` 同线程；不一致即 debug
  下断言失败。它不提供锁、不阻止并发，只是开发期把「方法被用错线程/序列」提前炸出。
  因为 avbase 每条逻辑 sequence 都绑一条专享线程（第 5 站、docs/04 §2），线程身份就是
  Chromium sequence 令牌的忠实替身。

  **典型用法**：把守卫声明为成员，在「只允许在某序列上调」的方法开头断言，对象要换线程时先 Detach：

  ```cpp
  class Decoder {
   public:
    void Decode(Buffer buf) {
      DCHECK_CALLED_ON_VALID_SEQUENCE(checker_);  // 只允许在解码序列上调用
      // ...
    }
    void OnTransferToWorker() {
      DETACH_FROM_SEQUENCE(checker_);             // 对象即将换线程，先解绑
    }

   private:
    SEQUENCE_CHECKER(checker_);   // 成员：记录“我该在哪条线程上被用”
  };
  ```

  **两个要点**：① 一次性绑定、不能迁移，想换线程必须先 `DETACH_FROM_SEQUENCE`；
  ② 宏展开成 `mutable` 成员（`sequence_checker.h:48`），**只能作类成员**；局部场景
  直接用 `base::SequenceChecker sc;`（见 `threading_demo.cc` 第 (h) 段的运行时演示）。
- `types/expected.h`：`-fno-exceptions` 下的返回值通道。`value()` 在错误态直接
  `CHECK` 失败（第 163-192 行），头文件第 11-13 行把这条写成了 IMPORTANT。

---

### 第 8 站 · 测试地基 —— `base/test/task_environment.{h,cc}`（182 行）

**读它之前先读 `tests/unit/base/task_environment_unittest.cc`。** `TaskEnvironment`
是全部单测的底座：`TimeSource::MOCK_TIME` 让时钟变虚拟，`FastForwardBy()`
按 (ready_time, insertion_sequence) 排序推进 —— 那个排序正是 `TaskQueue`
保证的（见第 5 站），两者是配套的。

---

## 3. 抄作业区 —— 8 段值得模仿的写法

这些是复核时确认真实存在的段落，按"学什么"排列：

| # | 位置 | 学什么 |
|---|---|---|
| 1 | `scoped_refptr.h:168-185` | 注释写"为什么这么选"，不写"这段代码做了什么" |
| 2 | `check.h:19-50` | 两类断言分开；断言强度写进宏，不写进调用点 |
| 3 | `weak_ptr.h:126-132` | 崩溃条件与调试断言分层，别混成一个 `DCHECK` |
| 4 | `task_queue.cc:106-117` | 并发注释写出失效时的具体症状（含原版病灶编号） |
| 5 | `time.h:178-201` | 不变量的证明集中一处，不在调用点各写一遍 |
| 6 | `types/expected.h:163-192` | `-fno-exceptions` 下，前置条件用 `CHECK` 落成代码 |
| 7 | `bind.h:1-18` | 降级/子集边界写进文件头，让后来的人不用猜 |
| 8 | `task_queue.h:24-41` | 偏离上游设计（collapse 三个类）时，说明触发拆回的条件 |

第 4 条还额外教一件事：注释里引用**原版的具体 bug**（"release() 永久挂起"），
比抽象的"需要同步"有用得多，因为它给了读者一个可验证的失效模型。

---

## 4. 陷阱区 —— 复核后确认的 5 处"别照抄"

### T1 · 文档和注释里 7 处引用了**不存在的门禁规则**（两轴都没发现）

`check_invariants.py` 当前活跃规则只有 **C1, C4, C5, C16, C22–C30**。但下面这些
地方引用了已经删除或从未存在过的编号：

| 引用处 | 引用的规则 | 实际状态 |
|---|---|---|
| `STYLE.md:48` | C9（`goto` = 0） | 已删除 |
| `STYLE.md:52` | C14（头文件禁 `using namespace`） | 已删除 |
| `STYLE.md:179` | C2（单函数 ≤ 80 行） | 已删除 |
| `STYLE.md:181` | C3（单类 public 方法 ≤ 12） | 已删除 |
| `docs/02:362` | C8（`LIBAV*_VERSION_*` 白名单） | 已删除 |
| `docs/06:483` | C8 | 已删除 |
| `base/memory/weak_ptr.h:142` | C19（弱指针必须是最后一个成员） | **从未存在过** |

已删除的九个是 C2, C7–C9, C11, C14, C17, C18, C20, C21 —— 见 `check_invariants.py`
自己的 docstring（第 28-30 行），那里还写了「IDs are not reassigned so history
stays greppable」。

**最后一条最危险**：`weak_ptr.h:140-143` 声明「MUST be the last member... rule C19
enforces this ordering」，而 `git log -S "C19" -- tools/check_invariants.py` **零结果**
—— 这条规则从来没被写过。「必须是最后一个成员」这条纪律仍然成立（析构顺序），
但**没有任何东西在守它**。把它当机器保证，就会漏掉一个真实的生命周期 bug。

**给初学者的规则**：看到 `check_invariants Cxx` 这样的引用，先去
`tools/check_invariants.py` 里 grep 那个编号确认它还在。

### T2 · `task_runner_util.h` 自相矛盾（**两轴独立命中同一处** —— 最实质的发现）

第 18-20 行承诺：

```
// Runs |task| on |task_runner| and delivers its return value to |reply| on the
// calling sequence.
```

第 55-56 行却说：

```
// The reply runs on |task_runner| too. Callers that need it on their own
// sequence pass a runner for that sequence, matching Chromium.
```

第 60 行的代码把 `done` 投回**同一个 `task_runner`**。所以：

- 头注释说的是 Chromium 的真实语义（reply 回调用方 sequence）；
- 行内注释说"matching Chromium"，但描述的行为**不是** Chromium 的行为；
- 代码实现了行内注释描述的行为。

三处里至少两处是错的。而且这个 API **全树只有 `tests/unit/base/task_queue_unittest.cc:211`
一个消费者**，没有任何生产调用方（`docs/03:181`、`docs/04:245` 只是文档里的示意）。

**这决定了修它的风险等级**：现在改是安全的（无生产调用方），但照着头注释去用
就会踩坑 —— 你会以为 reply 在自己的 sequence 上跑，实际它在目标 runner 上跑。
这正是规格把它标 ★ 的原因（"异步 API 的返回值通道"）。

### T3 · `ref_counted.h` 的注释被自己的代码否定了

第 88 行写着 `// Single-sequence ref counting. No atomic overhead on
AddRef/Release.`，但 `RefCounted<T>` 继承的 `RefCountedBase`（第 42-61 行）里
`ref_count_` 是 `mutable std::atomic<int>`,`AddRef()` 用的是
`fetch_add(1, std::memory_order_relaxed)`。

`relaxed` 去掉的是**顺序保证**（不需要栅栏），不是**原子性** —— x86 上它仍然是
一条 `lock xadd`。**relaxed ≠ 免费。** 注释想表达的应该是"无同步开销"，写成
"无原子开销"会让初学者以为这里不是原子操作。

顺带：`RefCountedBase`（第 42-61 行）与 `RefCountedThreadSafeBase`（第 63-84 行）
**几乎逐字相同**，差别只有 `HasOneRef()` 一个函数；连内存序参数都一模一样。
想抽公共基类的话，这里能省掉近 40 行。

### T4 · `logging.cc:49` 有一个裸 `delete`

```cpp
LoggingDelegate*& DelegateSlot() {           // :24-27
  static LoggingDelegate* slot = nullptr;
  return slot;
}
void SetDelegate(std::unique_ptr<LoggingDelegate> delegate) {
  std::lock_guard<std::mutex> lock(DelegateMutex());
  delete DelegateSlot();                     // :49
  DelegateSlot() = delegate.release();
}
```

`STYLE.md` §1 与 `docs/01` G5 都规定：核心代码禁止直接使用 `new`/`delete`，
白名单只有 `base/memory/`。这里在 `base/` 但不在 `base/memory/`，所以是偏离。

**但要看上下文再判断**：文件头第 17-19 行给了理由 —— 用函数内 static 而不是
堆上泄漏，因为 LeakSanitizer 干净是发版门槛（`docs/07` §9.1）。而且对外 API
`SetDelegate(std::unique_ptr<...>)` 的所有权边界是**对的**，裸指针只活在
`.cc` 内部。所以这是"有注释说明的内部取舍"，不是"随手 new 忘了 delete"。
改法：`static std::unique_ptr<LoggingDelegate> slot;`。

### T5 · `default_tick_clock.cc:10` 的 `new` 没有理由

```cpp
const DefaultTickClock* DefaultTickClock::GetInstance() {
  static const DefaultTickClock* instance = new DefaultTickClock();  // :10
  return instance;
}
```

和 T4 是同一类问题，但**少了一个理由**：这里完全可以直接
`static const DefaultTickClock instance;` —— 既不堆分配也不泄漏，语义相同。
全文件只有 14 行，一眼能看完，是练手改这类问题的好起点。

### 附：两处"看着像违规、其实不是"

这两处容易被误判，说明**规则要看全再下结论**：

- `scoped_refptr` / `raw_ptr` 的单参构造函数**没有** `explicit`，看着违反 STYLE §5；
  但 `scoped_refptr.h:31-47`、`raw_ptr.h:18-36` 写了理由，且 STYLE §10 要求与
  Chromium 同名同义 → **不算违规**。
- `bind.h` / `callback.h` 用了 `std::shared_ptr`，而 STYLE §1 写的是"**不推荐**"
  而非"禁止"，理由是 `RepeatingCallback` 需要廉价拷贝 → 属**可辩护的取舍**。

---

## 5. 规格 vs 实现对照表

规格来源：`docs/02` §3.2「需要实现的清单」（第 107-143 行，28 个文件）。

**量化两条**：

| 规格说法 | 实际 | 偏差 |
|---|---|---|
| 约 28 个文件 | 44 个代码文件 | **+16**（清单没算分解件） |
| 4000 行（含测试） | 4069 行代码 | **+2%**（准得惊人） |
| 2500 行测试 | 182（`base/test/`）+ 2115（`tests/unit/base/`）| 位置不在规格说的地方 |

**归属声明**：`docs/02` 第 104 行要求"每个文件头注明 `API mirrors Chromium's
base/xxx.h`"，33 个头文件中 **25 个有**。缺的 8 个：

- **在 §3.2 清单内却缺声明（2 个，明确违反规格）**：
  `synchronization/condition_variable.h`、`synchronization/atomic_sequence_number.h`
- **清单外但有对应 Chromium 文件（4 个，属疏漏不算违约）**：
  `functional/callback_forward.h`、`memory/ptr_util.h`、
  `synchronization/atomic_flag.h`、`time/default_tick_clock.h`
- **自研件（1 个，不违规）**：`task/task_queue.h`（头注释已说明是 avbase 自己的设计）
- **纯工具（1 个）**：`base_export.h`

**分类结果**：

| 类别 | 条目 |
|---|---|
| **有意降级，活文档有记录** | `message_loop/` 三项 → `docs/08` §5.1 R2 的 L2 行；代价登记在 `docs/archive/PROGRESS-rounds-1-9.md:1295` |
| **记录在归档，活文档无** | `containers/circular_deque.h`、`feature_list.h`+`features.h`、`trace_event/`、`files/` |
| **纯 drift（无任何记录）** | `test/mock_callback.h`、`test/scoped_feature_list.h`、`test/test_waitable_event.h` |
| **换形态落地（代码有注释，文档无）** | `task/single_thread_task_runner.h` → `sequenced_task_runner.h:37` 的 `using` 别名，附"avbase 不在类型层区分"的理由 |
| **部分落地** | `WaitableEvent::WaitMany` 缺；`callback_helpers.h` 缺 `IgnoreResult()`、`SplitOnceCallback()`（有 `DoNothing()`、`DoNothingRepeating()`、`ScopedClosureRunner`）；`observer_list.h` 缺 `ScopedObservation`；`thread.h` 缺 `WasStartedSuccessfully()`（改为 `IsRunning()`） |
| **已按备注落地** | `SequenceChecker` 三宏；`raw_ptr` 注解别名 + 预留 BackupRefPtr；`expected` 在 C++23 下 `using std::expected` |

**关于"超范围"**：几乎都不是。清单里没有 `task_queue` 这一行，但它是
**清单项的实际实现形态**（`task_queue.h:26-29` 明确说它 collapsed 了
MessageLoop/MessagePump/TaskQueue）；`atomic_flag.h` 未列，但 `docs/04` §9 D3
要求"原子标志 + interrupt_callback"。**是清单漏列，不是代码越界。**

---

## 6. 改 `base/` 要跑的门禁

```bash
# 1) 风格与静态检查（fmt 是 clang-format 18.1.3，全量 --dry-run --Werror）
tools/gate_local.sh --jobs fmt lint

# 2) 结构不变量：C1(500 行) / C5(禁平台头) / C22(base 不得 include player) /
#    C23(80 列) / C24 / C25
python3 tools/check_invariants.py

# 3) 单测
ctest --test-dir build/<preset> --output-on-failure

# 4) 微基准（改了 functional/ memory/ task/ 必须跑）
#    bench 目标用 -DAVBASE_BUILD_BENCH=ON -DAVBASE_BUILD_TESTS=OFF 配置
python3 tools/check_bench_regression.py --baseline tools/bench-baseline.jsonl \
    --current <out.jsonl> --fail-on-regression
```

**⚠️ 改任何测量类代码前，先确认被测二进制是新的**：

```bash
python3 tools/check_binary_freshness.py --root build/<preset>
```

退出码：`0` 通过 · `1` 被测物有问题 · `2` 无从判断（环境缺口不是产品回归）。
这个工具比较两件事：二进制 vs 它链接的库，以及**二进制 vs 喂进这些库的源码**。

---

## 7. 自测题

读完上面八站后，试着回答（答案都在引文里）：

1. `CHECK` 和 `DCHECK` 的判据是什么？举一个你手头代码里该用 `CHECK` 却用了 `DCHECK` 的例子。（答案见 §8）
2. `RefCounted<T>` 真的没有原子开销吗？依据是哪一行？
3. `MakeRefCounted` / `WrapRefCounted` / `AdoptRef` 三个动词，选错会怎样？
4. 规格说 `bind.h` 约 800 行，实际 311 行 —— 差的那些去哪了？依据在哪一行？
5. `PostTaskAndReplyWithResult` 的 reply 到底跑在哪个 sequence 上？头注释和代码一致吗？它有生产调用方吗？
6. 为什么 `TaskQueue` 能取代规格里的 `message_loop/` 三项？什么条件下要拆回去？
7. 你想验证「弱指针必须是最后一个成员」这条规矩，能靠门禁吗？

---

## 8. 什么时候用 `CHECK`，什么时候用 `DCHECK`

> 本节回答 §7 自测题第 1 题。所有引文都指向真实行号。

### 8.1 `STYLE.md` 的旧措辞（**已于 2026-10-23 修正**，理由保留在此）

`STYLE.md` §6 原来写的是：

```
CHECK()  = 不可恢复的编程错误，立即崩溃并打印栈（release 也生效）。
DCHECK() = 可恢复的编程错误，debug 生效。
```

第二行的「可恢复」会误导，已改为「release 里整行被编译掉」并补上三问判据表。
**在 avbase 里，`DCHECK` 失败同样是 abort**——`check.h:35` 就是
`#define DCHECK(condition) CHECK(condition)`，debug 下它和 `CHECK` 是同一个东西，一样
`LOG(FATAL)` 一样 `std::abort()`（`check.cc:15-22`）。

「可恢复」的真实含义是：**release 构建里整行被编译掉，不为它付任何代价**。不是「失败
后程序还能继续跑」。下面这条推论才是要紧的：

> release 里 `DCHECK` 的条件**根本不存在**。所以如果一个条件在 release 里也必须成立，
> 你只能选 `CHECK`；如果它只是内部对账，release 里不成立也死不了人，才轮到 `DCHECK`。

### 8.2 判据（按顺序问三个问题）

**问题一：这个条件的真假，取决于不可信输入吗？**
（媒体码流、容器头、网络响应、用户/调用方传进来的参数值）

是 → **两个都不要用**。走 `base::expected<T, MediaError>` 把错误返回给上层。
`STYLE.md:212` 是强制项：

> 不允许用 `DCHECK` 代替对不可信输入（媒体数据）的校验 —— 媒体数据一律走
> `base::expected` 返回错误。

断言是给「程序员写错了」用的，不是给「数据坏了」用的。一个畸形 MP4 让播放器崩掉是
bug；让它优雅报错才是功能。

**问题二：违反之后继续往下跑，后果是 UB / 内存破坏 / 安全边界失守 / 静默数据损坏吗？**

是 → **`CHECK`**。典型形状：

| 形状 | 例子 |
| --- | --- |
| 依赖注入的必需项 | `renderer_impl.cc:43-47`（`CHECK(deps_.media_task_runner)` 等 5 条） |
| API 前置契约 | `expected.h:167-190`（`CHECK(has_value_)` / `CHECK(!has_value_)`） |
| 后续算式的前提 | `audio_bus.cc:22-23`（`CHECK_GT(channels, 0)`、`CHECK_GT(frames, 0)`） |
| 句柄/资源的存在性 | `video_frame.cc:240`（`CHECK(frame->allocation_)`）、`decoder_buffer.cc:37` |

共同点：**违反之后不是「行为有点怪」，而是「读到野指针 / 越界 / 未初始化内存」**。
比如 `expected.h` 的 `CHECK(has_value_)`——release 下如果放过，`value()` 就会去读一个
从未被构造的 `T`，那不是返回垃圾，是 UB。再看 `renderer_impl.cc:43`：
`deps_.media_task_runner` 是空的话，崩点在几十帧之后的第一次 `PostTask`，离真正的病
因十万八千里。这类「延迟爆雷」必须用 `CHECK` 在最早的位置钉住。

**问题三：都不是？这段路径每帧/每样本都跑吗？**

→ **`DCHECK`**。典型形状是状态机与内部对账：

- `renderer_impl.cc:396` `DCHECK(initialized_)` —— 方法调用顺序契约，只有自己人会犯
- `renderer_impl.cc:518-519, 556` `DCHECK_EQ(pending_video_flushes_, 0u)` /
  `DCHECK_GT(pending_video_flushes_, 0u)` —— flush 计数器的账目
- `ref_counted.h:45,51,66,75` `DCHECK_GE(ref_count_..., 0)` /
  `DCHECK_EQ(ref_count_..., 0)` —— 引用计数账目，捕捉 use-after-free / 重复释放

这些条件违反时，程序状态**仍然自洽**（计数器多算了一次），只是逻辑错了。debug 下抓
住它，release 下省掉开销，是划算的交换。

### 8.3 代价（上一轮实测，非估算）

| 写法 | 单次耗时 |
| --- | --- |
| 裸 `if (!x) return;` | ≈ 0 ns |
| `CHECK_EQ(a, b)` 通过 | ≈ 43 ns |
| `DCHECK_EQ(a, b)`（关闭时） | 0 ns |

43 ns 来自 `CheckOpStreamHelper` 构造时带上的 `std::ostringstream`（即使条件为真也要构
造）。所以「每帧 × 每个平面 × 每个样本」的位置放 `CHECK` 要算账——但**别因为怕开销就
把问题二里的 `CHECK` 降级成 `DCHECK`**，那是拿 UB 换 43 ns。

### 8.4 两个求值陷阱（release 下才会露出）

1. **`DCHECK_EQ(a, b)` 的 `a`、`b` 在 release 下完全不求值。**
   `check.h:44-49` 把它们统统定义成 `DCHECK(true)`。所以**绝不能**往里塞有副作用的
   调用：`DCHECK(Init())` 在 release 里意味着 `Init()` 根本没跑过。
2. **但 `DCHECK(c) << SideEffect()` 里 `<<` 右侧仍然会求值。**
   `check.h:43` 把 `DCHECK(c)` 变成 `NullCheckStream()`，它只「吞掉值」，参数还是得
   先算出来。这一条 `tests/unit/base/check_unittest.cc` 里有断言钉着。

### 8.5 自己跑一遍（30 秒）

下面两段代码可以直接对着已经编译好的 `libavbase_base.a` 编，用来把上面所有说法变成眼睛
看得到的事实：

```cpp
// check_demo.cc —— CHECK 失败长什么样
#include <cstdio>
#include "base/check.h"
int main() {
  CHECK(2 + 2 == 5) << "2+2 should be 4, got " << (2 + 2);
  printf("not reached\n");
}
```

```cpp
// dcheck_demo.cc —— DCHECK 关掉后条件「不存在」到什么程度
#include <cstdio>
#include "base/check.h"
int calls = 0;
int Bump() { ++calls; return 1; }
int main() {
  DCHECK_EQ(Bump(), Bump());
  printf("DCHECK_EQ evaluated Bump() %d time(s)\n", calls);
}
```

```
cd <repo> && clang++ -std=c++20 -I. /tmp/chk/check_demo.cc \
    build/full/lib/libavbase_base.a -o /tmp/chk/check_demo
cd <repo> && clang++ -std=c++20 -I. -DAVBASE_ENABLE_DCHECK=1 /tmp/chk/dcheck_demo.cc \
    build/full/lib/libavbase_base.a -o /tmp/chk/dcheck_on
cd <repo> && clang++ -std=c++20 -I. /tmp/chk/dcheck_demo.cc \
    build/full/lib/libavbase_base.a -o /tmp/chk/dcheck_off
```

实测输出：

```
$ ./check_demo
[FATAL check.cc:20] Check failed: 2 + 2 == 5. 2+2 should be 4, got 4
exit=134                      # 128 + 6，SIGABRT

$ ./dcheck_on
DCHECK_EQ evaluated Bump() 2 time(s)

$ ./dcheck_off                # 这就是 release 构建
DCHECK_EQ evaluated Bump() 0 time(s)
```

三行输出把整节讲完：第一行说明「失败 = 一条可定位的消息 + abort」，第二、三行的差说明
「release 里 DCHECK 连同它的操作数一起消失」。

### 8.6 补白：测试名 `ChainReturnsItself` 到底在测什么

`tests/unit/base/check_unittest.cc:100` 有个测试叫 `ChainReturnsItself`，光看名字很劝退。它测的是
`NullCheckStream::operator<<` 的一条契约——**每接一次 `<<`，它都把「自己」（同一个对象）交回去**，
于是下一发 `<<` 还能挂在上面。

为什么必须这样？因为 `<<` 是左结合的：

```
stream << 1 << "x" << 2.5
// 实际被编译器拆成：
((stream << 1) << "x") << 2.5
```

每一步 `<<` 都是一次函数调用，它的**返回值**会当作下一个 `<<` 的左操作数。如果 `operator<<`
不返回「自己」，链条在第二次 `<<` 就断了，编译都过不了。`NullCheckStream` 的写法（check.h:85-87）：

```cpp
template <typename T>
NullCheckStream& operator<<(const T&) { return *this; }  // 把自身交回去
```

注意返回类型是 `NullCheckStream&`（引用），`*this` 就是「这个对象自己」。返回引用意味着还回去的
是**同一个对象**，不是副本。

测试在断言什么：

```cpp
NullCheckStream stream;
EXPECT_EQ(&(stream << 1), &stream);                              // (a)
EXPECT_EQ(&(stream << 1 << std::string("x") << 2.5), &stream);   // (b)
```

- `&(stream << 1)` 取的是「`stream << 1` 这个返回值（一个引用）指向的对象」的地址。
- 因为返回的是 `*this` 即 `stream` 本身，地址就等于 `&stream`。
- (b) 的三连 `<<` 也只返回这一个 `stream`。

所以它断言的不是「还能继续写」，而是「每次返回的是同一个对象，而不是副本」——名字
`ChainReturnsItself` 就是「链式调用返回自身」的直译。

**为什么 `NullCheckStream` 非这么干不可**：DCHECK 关闭时，`DCHECK(x)` 宏展开成 `NullCheckStream()`
（check.h:43），于是你写的 `DCHECK(x) << foo << bar` 在 release 里变成
`NullCheckStream() << foo << bar`。这是个临时对象，要让后面的 `<< foo << bar` 还能编译、还能
「什么都不做地过去」，这个临时对象就得能接 `<<` 并交还自己继续接下一个 `<<`。`return *this`
正是让这一串无操作链成立的钥匙。

**对照 `CheckOpStreamHelper`**：同一个套路（check.h:69-74 的 `operator<<` 也 `return *this`），
语义不同——它真把字攒进 `ostringstream`，且**最后那一发 `<<` 返回的临时对象在分号处析构**，
析构函数（check.cc:15-22）才真正判条件、打 FATAL、abort。`NullCheckStream` 的 `operator<<`
永远不干活，是给「关掉的 DCHECK」用的空壳。

> 记忆锚点：这和 `std::cout << a << b` 是**同一个 C++ 习惯**——`<<` 每次把「流自己」交回来，你才能
> 一直接下去。`NullCheckStream` 就是个「什么都不记、只负责把链条传下去」的流。

### 8.7 补白：测试名 `DisabledDcheckSkipsConditionButNotStream`

这个名字把两件事并排钉在一起，名字本身就是结论：**「DCHECK 关掉时，跳过的是条件，跳不过的是流」**。

看 `tests/unit/base/check_unittest.cc:149` 的实测：

```cpp
int condition_calls = 0, stream_calls = 0;
auto condition = [&] { ++condition_calls; return 0; };
auto streamed  = [&] { ++stream_calls;    return 0; };

DCHECK(condition()) << streamed();

EXPECT_EQ(condition_calls, 0);   // 条件：0 次
EXPECT_EQ(stream_calls, 1);      // 流：1 次
```

只差一个位置，结果天差地别。原因是宏展开的形态不同（check.h:43-49）：

- `DCHECK(condition())` 在关闭时展开成 `NullCheckStream()`。`condition()` 是宏的**参数**，而展开体里
  压根没引用这个参数——于是 `condition()` 这两个字从源码里被**整段抹掉**，编译期就不存在，自然 0 次调用。
  同理 `DCHECK_EQ(a, b)` 展开成 `DCHECK(true)`，所以 `a`、`b` 也都一起消失（§8.5 的 `dcheck_off` 跑出
  0 次就是这个）。
- 但 `<< streamed()` 在宏**外面**。展开后整句变成 `NullCheckStream() << streamed()`，`streamed()` 是
  `operator<<` 的参数，得先算出来才能传进去。所以哪怕 `NullCheckStream` 只是把它吞掉不干事
  （check.h:85-87 `return *this;`），`streamed()` 这一发**确实跑了 1 次**。

一句话区分：

| 写法 | 在源码里的身份 | 关闭 DCHECK 后 |
| --- | --- | --- |
| `DCHECK(cond)` 里的 `cond` | 宏参数 | 被抹掉，不求值 |
| `DCHECK_EQ(a, b)` 的 `a`/`b` | 宏参数 | 被抹掉，不求值 |
| `DCHECK(c) << x` 的 `x` | 宏外面的 `<<` 操作数 | 照样求值（被空壳吞掉） |

> 推论（写代码要记住）：**绝不要**在 `DCHECK(c) << Expensive()` 里放有副作用或很贵的 `<<` 内容——
> 它在 release 里照样执行，只是结果被丢掉。要省就省整个条件，别指望「反正 DCHECK 关了就白跑」。

> 附带一个编译细节：测试里 `condition` 那行加了 `[[maybe_unused]]`，不是装饰——正因为 `DCHECK(cond)`
> 展开后把 `condition` 的引用整个删了，编译器会看到「定义了却没人读」的变量，在 `-Werror` 下直接编不过。
> 这层属性正好反向证明了「条件确实被跳过了」。

### 8.8 补白：测试名 `FailingCheckAborts`

这个名字是「主语 + 谓语」结构：**「失败（条件为假）的 CHECK，会中止进程（abort）」**。

```cpp
TEST(CheckDeathTest, FailingCheckAborts) {
  EXPECT_DEATH(CHECK(false), "Check failed");
}
```

三个词拆开：
- **Failing** = `CHECK` 的条件为假。`CHECK(false)` 字面就是 `false`。
- **Check** = 被测的 `CHECK` 宏。
- **Aborts** = 进程被中止——不是返回、不是抛异常，而是 `std::abort()`（SIGABRT，退出码 134）。

为什么用 `EXPECT_DEATH` 这种特殊写法，而不是直接 `EXPECT`？因为 `CHECK(false)` 一旦触发，会走到
check.cc:20 的 `LOG(FATAL) << ...` 之后 `std::abort()`——这会把**整个进程**干掉，包括测试框架自己。
所以「它 aborts」这个断言不能在同一个进程里做。gtest 的死亡测试把那段代码放进一个**子进程**（POSIX 下
`fork`）里跑，观察子进程是否「死掉」（被信号终止），并匹配它打到 stderr 的日志。这就是文件第 175 行注释
「失败路径：只能在子进程里观察」的含义。

几个配套测试（都在 188-231 行）都是同一个 abort 路径的不同侧面：
- `MessageNamesTheCondition`（:193）：日志不只写文件名，还把条件原文写进去，例如 `Check failed: ready`——
  这点对事后从日志定位至关重要。
- `FailingComparisonAborts`（:208）：`CHECK_EQ(a, b)` 底层就是 `CHECK((a) == (b))`，所以失败同样走到
  同一处 abort。
- `NotReachedAborts`（:214）：`NOTREACHED()` **永远开着**，没有 `DNOTREACHED()`——「这条路不该走到」在
  release 里也不该被悄悄放过。

还有个细节：匹配串用的是 `"Check failed"` 这种**纯子串、不含正则元字符**。注释（:184-186）解释：gtest 在
POSIX 用 POSIX 扩展正则，但在 Windows 用自己更简单的引擎，两者对转义的理解不一致；用纯文字子串能跨平台
稳定匹配。

> 记忆锚点：`CHECK` 失败 = 一条能定位的消息（check.cc:20 的 `LOG(FATAL)` 带着条件原文和文件行号）
> + `abort()`。它可以在 debug 下被 `EXPECT_DEATH` 在子进程里验证，但**没有「捕获异常继续跑」这一说**——
> 在 `-fno-exceptions` 构建里，`LOG(FATAL)` 之后就是 `std::abort()`，进程到此为止。

### 8.9 补白：如何单独运行 `check_unittest.cc`

这个文件不是独立二进制——`tests/unit/base/*.cc` 被 glob 进同一个 `base_unittests` 目标（见
`tests/unit/base/CMakeLists.txt`）。要「只跑这个文件」靠两层：

**1. 只编译这个目标**（省去全树重建）：

```sh
cmake --build build/full --target base_unittests
```

**2. 只跑这个文件的用例**（gtest 按 `套件.用例` 过滤；本文件有四个套件）：

```sh
build/full/bin/base_unittests \
  --gtest_filter='CheckTest.*:NullCheckStreamTest.*:DcheckTest.*:CheckDeathTest.*'
```

只跑单个用例就把 `*` 换成具体名字，例如
`--gtest_filter='CheckDeathTest.FailingCheckAborts'`；`--gtest_list_tests` 可先列全貌。

**一个重要前提**：`build/full` 是 DCHECK *打开* 的构建，所以 `DisabledDcheckSkipsConditionButNotStream`
（`#else` 分支）不会被编译进去——上面那条过滤只会跑出 12 个。要跑那个测试，得用 DCHECK *关闭* 的构建
（CMakePresets 里 `release` 预设 `AVBASE_ENABLE_DCHECK=OFF`）：

```sh
cmake --preset release && cmake --build build/release --target base_unittests
build/release/bin/base_unittests --gtest_filter='DcheckTest.*'
```

> 记一条：跑之前先 `python3 tools/check_binary_freshness.py --root .` 确认二进制不比源码旧——否则
> 跑的是上一个二进制，假绿（之前 ctest 736 vs 784 那类事故都是它）。

### 8.10 一句话记忆

> `CHECK` 保护「运行中的程序」，`DCHECK` 保护「写代码的同事」；
> 来自外部世界的数据，两个都不保护——那归 `base::expected` 管。

---

## 附 A · 轴一报告：标准符合性

> 以下为子代理报告原文，经轻度清理。**未与轴二合并、未重排。**

### (a) 文档化标准违规

工具轴干净：`python3 tools/check_invariants.py` → `all rules pass`，C23 与基线持平，
`base/` 无 C1/C2/C3/C5/C22/C24/C25 问题。

**硬违规**

1. `base/logging.cc:24-27,49` — `LoggingDelegate*& DelegateSlot()` + `delete
   DelegateSlot();`。STYLE §1：核心代码禁止直接使用 `new`/`delete`；裸 `T*` 表示
   所有权禁止（非拥有用 `raw_ptr<T>`）。应改 `static std::unique_ptr<LoggingDelegate>`。
2. `base/time/default_tick_clock.cc:10` — `static const DefaultTickClock* instance =
   new DefaultTickClock();`。同上 §1；`static const DefaultTickClock instance;` 既不
   分配也不泄漏，语义相同。
3. `base/types/expected.h:18-22` — `#include <new>` → `"base/check.h"` →
   `<type_traits>` `<utility>`。STYLE §3：include 顺序为 C 系统 → C++ 标准 → 本项目，
   每块空行；此处项目头插在标准头之间。
4. `base/observer_list.h:111` — `Entry { ObserverType* observer; bool alive; }` 非拥有
   裸指针。§1：非拥有用 `raw_ptr<T>`（`weak_ptr.h:137`、条件变量
   `condition_variable.h:48` 都做对了）。
5. `base/observer_list.h:110-113`、`base/types/expected.h:221`、`base/memory/raw_ptr.h:90`
   — 成员无声明处默认值。§5：结构体/POD 成员声明处给默认值 `int x{0};`。
6. `base/task/task_runner_util.h:18-19` vs `:60` — 注释称 reply "delivers to |reply| on
   the calling sequence"，代码把 `done` 投回 `task_runner`。§4 注释必须准确 + §7 跨
   sequence 只用 PostTask。`tests/unit/base/task_queue_unittest.cc:211` 单队列跑，
   测不出这个差别。
7. `base/threading/thread.h:54-56` — 注释写 "unless |flush| is false"，`Stop()` 无形参。
   §4 陈旧注释比无注释更误导。

**判断项**

8. `base/functional/bind.h:294`、`base/functional/callback.h:183` — `std::shared_ptr`。
   §1 措辞是"不推荐，用 `scoped_refptr`"，非禁止；`RepeatingCallback` 要廉价拷贝，
   此处属可辩护取舍。

**抑制项**：`scoped_refptr`/`raw_ptr` 的单参构造不加 `explicit`、类名小写，看似违反
§5「单参构造必须 explicit」与 §2「类型 CamelCase」，但 `scoped_refptr.h:31-47`、
`raw_ptr.h:18-36` 已写明理由，且 STYLE §10 要求与 Chromium 同名同义 → 不算违规。

### (b) 基线气味（均为判断项）

1. **Duplicated Code** — `bind.h:262-270` 与 `:284-292` 逐字相同（`Traits`/`RunType`/
   `CallbackSig`/`bound_tuple` 四步）。
2. **Duplicated Code** — `ref_counted.h:42-61` 与 `:63-84`：`AddRef()`、`~` 的
   `DCHECK_EQ`、`ReleaseImpl()`、`ref_count()`、`ref_count_{0}` 全部重复；两个派生类
   `:95-99` / `:124-128` 的 `Release()` 也相同。
3. **Duplicated Code** — `sequence_checker.h:27-35` 与 `weak_ptr.h:43-51`：同一段
   `compare_exchange_strong(expected{}, id)` 绑定序列的写法出现两次。

### (c) 值得抄的段落

见正文 §3 抄作业区。

**「初学者该学到什么」**：所有权只用 RAII 容器表达，`delete` 出现即是信号；不泄漏
的单例用函数内 `static` 对象，不需要 `new`；include 分块是给人看的导航，插花等于
取消导航；非拥有指针用 `raw_ptr<T>`，让类型系统替你声明"我不拥有"；成员声明处给
默认值，构造漏设就有确定值而非 UB；注释与代码冲突时二者必有一错，先确认哪个是
权威再改，并补一个能区分两种语义的测试；改签名时同步改注释；"不推荐"的规则要留
取舍理由，不要默默违反。气味：公开 API 命名先拼写检查再提交；同样的形状出现两次
就该抽出来。

---

## 附 B · 轴二报告：规格符合性

> 以下为子代理报告原文，经轻度清理。**未与轴一合并、未重排。**

### (a) 规格要求了但缺失/只部分落地

**量化两条**：L104「约 28 个文件 / 4000 行（含测试）」→ 实际 44 个 `.h/.cc`、
4111 行；行数误差 2%，**文件数 +16 是清单没算分解件**。L145「2500 行测试」不在
`base/`：`base/test/` 仅 182 行，2115 行在 `tests/unit/base/`。
L104「每个文件头注明 `API mirrors Chromium's base/xxx.h`」→ 25 个头有该行，
**§3.2 表内已落地的 25 个里缺 2 个**：`condition_variable.h`、
`atomic_sequence_number.h`；表外还缺 4 个。

分类：

- **有意降级且已记录**：`message_loop/` 三项 → `docs/08` §5.1 L2 预案（L203）
  + `docs/archive/PROGRESS-rounds-1-9.md:1295`「L2 无 message pump」逐条登记代价。
- **记录在归档里、活文档无**：`circular_deque.h`、`feature_list.h`+`features.h`、
  `trace_event`、`files/`。
- **纯 drift（无任何文档记录）**：`test/mock_callback.h`、
  `test/scoped_feature_list.h`、`test/test_waitable_event.h`。
- **换形态落地（代码有注释，docs 无）**：`single_thread_task_runner.h` →
  `sequenced_task_runner.h:37` `using SingleThreadTaskRunner = SequencedTaskRunner;`。
- **部分落地**：`WaitMany` 缺失；`callback_helpers.h` 缺 `IgnoreResult()`、
  `SplitOnceCallback()`；`observer_list.h` 缺 `ScopedObservation`；`thread.h` 缺
  `WasStartedSuccessfully()`，改为 `IsRunning()`。
- **已按备注落地**：`SequenceChecker` 三宏；`raw_ptr` 注解别名 + 预留 BackupRefPtr；
  `expected` 在 C++23 下 `using std::expected`。
- **元缺口**：L145 指向「08 §5 R16」，但 `docs/08` 的 R16 是 Wayland 嵌入，
  base 的风险条目是 **R2** —— 规格自己的交叉引用漂了。

### (b) 超范围？—— 几乎都不是

`task_queue.{h,cc}`（292 行，最大 `.cc`）**正是清单项的分解形态**：
`task_queue.h:26-29`「This is avbase's message loop...」—— 即 L134 + L129 的实际实现。
`atomic_flag.h`：§3.2 未列，但 §9 D3 要求「原子标志 + interrupt_callback」→
**清单漏列，非代码越界**。`ptr_util.h`、`callback_forward.h`、
`default_tick_clock.h`/`simple_test_tick_clock.h`、`base_export.h` 全在 §7 目录树中 →
文档内部不一致，代码无额外范围。

### (c) 看着落地了，但语义偏离

- `bind.h` 311 行 vs L119「约 800 行」：**走的是"可先支持子集"**，证据在
  `bind.h:7`。
- `task_runner_util.h`：reply 被投回**同一个 task_runner**（`:60`），而 `:19` 的注释
  承诺「on the **calling** sequence」，`:55-56` 又自评「matching Chromium」——
  文件自相矛盾，且偏离 ★ 语义。
- 降级预案的 `std::jthread`+`latch` `SimpleTaskRunner` 并未照抄：实际是
  `std::thread`+`join`（`thread.cc:36`）与 Lock+ConditionVariable，但**接口形态
  保住了**。

### (d) 给新手的教训

代码紧贴设计文档（行数误差 2%，★ 项多数按备注落地），但**偏离靠"写下来"而非
"做完"来合法化**：`bind.h:7`、`sequenced_task_runner.h:37`、`raw_ptr.h:6-8` 把每个取舍
写进文件头；真正的项目级降级（L2 无 message pump）却只躺在
`docs/archive/PROGRESS-rounds-1-9.md:1295`，**活文档 `docs/12` §4 一条 base/ 都没有**
—— 这才是"有意降级 vs 静默漂移"的分界：前者有据可查，后者（mock_callback /
thread_pool / WaitMany）只能靠读代码发现。

---

## 附 C · 复核记录

按项目约定，子代理报告**逐条复核**。结果：绝大多数属实，3 处需要订正。

### 已复核属实

| 项 | 复核方式 | 结果 |
|---|---|---|
| check_invariants 全绿 | 实跑，522 文件 | ✅ 属实 |
| `logging.cc:24-27,49` 的 `delete` | 全文读 | ✅ 属实（但见订正 1）|
| `default_tick_clock.cc:10` 的 `new` | 读文件 | ✅ 属实 |
| `expected.h:18-22` include 插花 | 读文件 | ✅ 属实 |
| `observer_list.h:110-113` 裸指针 + 无默认值 | 读文件 | ✅ 属实 |
| `thread.h:54-56` 陈旧 `|flush|` 注释 | 读文件 | ✅ 属实 |
| `ref_counted.h` 两个 base 类重复 | 读 40-137 行 | ✅ 属实（且比报告更严重，见订正 2）|
| `bind.h:262-270` vs `284-292` | 读 255-300 行 | ✅ 属实 |
| `sequence_checker.h` vs `weak_ptr.h` 重复 | 两处对照 | ✅ 属实 |
| 归属声明 25/33 | 逐文件 grep | ✅ 属实，缺的 8 个已列全 |
| `WaitMany`/`ScopedObservation`/`IgnoreResult`/`SplitOnceCallback`/`WasStartedSuccessfully` 均缺 | grep base/ | ✅ 属实（唯一命中是 `bind.h:16` 的"不支持"清单）|
| `PostTaskAndReplyWithResult` 无生产调用方 | 全树 grep | ✅ 属实（唯一消费者是单测）|
| `task_queue.h:26-29` 的 collapse 说明 | 读文件 | ✅ 属实 |
| R2 是 base 的风险条目，R16 是 Wayland | 读 docs/08 §5 | ✅ 属实 |

### 订正 1 · `logging.cc` 的违规等级应从"硬违规"降为"有注释的内部取舍"

轴一把它列为硬违规，但忽略了两条上下文：文件头第 17-19 行**给了理由**（函数内
static 而非堆上泄漏，因为 LSan 干净是发版门槛）；对外 API
`SetDelegate(std::unique_ptr<...>)` 的**所有权边界是对的**，裸指针只活在这个
`.cc` 内部。属于"文档化的内部偏离"，与 T5（同样的问题但**没有**理由）性质不同。

### 订正 2 · `ref_counted.h` 的重复比报告更彻底

轴一建议"抽公共基类只留内存序差异"。实际读过第 42-84 行后发现：**内存序参数也
完全一样**（都是 `fetch_add(1, relaxed)` / `fetch_sub(1, acq_rel)`），差别只有
`HasOneRef()` 一个函数。另外第 88 行的注释「No atomic overhead」被自己的代码否定
（`ref_count_` 就是 `std::atomic<int>`）—— 见正文 T3。

### 订正 3 · 轴一当作正面例子的 C19，其实从未存在

轴一第 4 条称赞 `weak_ptr.h:140-143` 把纪律「交给 `check_invariants.py` 规则 C19
机器保证」。实际：`grep -n "C19" tools/check_invariants.py` **零命中**，
`git log -S "C19" -- tools/check_invariants.py` **零结果** —— 这条规则从来没被写过，
全树唯一提到 C19 的就是那条注释本身。它是**反面教材**（无人看守的纪律被写成
有人看守），不是正面例子。见正文 T1。

### 复核新发现 · 7 处指向已删除规则的引用

复核时顺手 grep 全树的 `check_invariants Cxx` 引用，发现 7 处指向已删除或从未存在
的规则（正文 T1 的表）。**两轴都没有报这一条** —— 轴一信任了注释，轴二专注于
`docs/02` §3 的清单，没查 `docs/02` §其他章节与 `docs/06` 的交叉引用。

### 复核方法备忘

- `--include=*.cc` 这类参数在 **zsh 下会被当 glob 展开而报
  `no matches found`**，必须改用仓库自带检索工具。
- BSD grep **不支持 `\b`**（词边界是 GNU 扩展），写成 `\b` 的正则会静默返回零命中
  —— 这类"安静的失败"比报错危险，因为它看起来像"确认不存在"。
- 量退出码不要接在管道后面（zsh 里 `$PIPESTATUS` 是大写的错，正确的是小写
  `$pipestatus`，且下标从 1 开始）。

// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by the LICENSE file.
//
// functional_demo.cc — 演示 base/functional/ 的回调与绑定机制全貌。
//
// 运行方式（CMake，examples/CMakeLists.txt 已加入 functional_demo 目标）：
//   cmake --build build/full --target functional_demo
//   ./build/full/examples/base/functional_demo     # Ninja 下路径通常如此
//
// 覆盖：
//   1. OnceCallback —— move-only，只能 Run 一次，Run 之后被消费
//   2. RepeatingCallback —— 可拷贝，可 Run 多次，拷贝共享同一份绑定状态
//   3. BindOnce —— 绑定自由函数 / lambda / 成员函数（裸指针、Unretained、
//      scoped_refptr、WeakPtr 四种接收者），0..N 个预绑定参数
//   4. BindRepeating —— 同上但可重复执行；值类型绑定参数在多次 Run 间不被移动
//   5. callback_helpers —— DoNothing / DoNothingRepeating / ScopedClosureRunner
//
// 贯穿主题：avbase 用 scoped_refptr（而非 std::shared_ptr）。回调经 BindOnce
// 绑定 WeakPtr 接收者时，目标析构后回调自动变成 no-op —— 这是从结构上消灭
// “对象已释放但回调还在飞” 类崩溃的核心机制（与 base/memory 第 2 部分呼应）。

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/functional/callback.h"
#include "base/functional/callback_helpers.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"

using namespace avbase;  // 示例而非 SDK：base/ 直接以 base:: 引用

namespace {

// ---- 一些被绑定的目标 ----

int FreeAdd(int a, int b) { return a + b; }

void TakeString(std::string sink, std::string* observed) {
  *observed = std::move(sink);
}

class Plain {
 public:
  void Note(int v) { last = v; }
  int Echo(int v) const { return v; }
  int last{0};
};

class Target : public base::RefCountedThreadSafe<Target> {
 public:
  void Note(int v) { last_ = v; }
  int last() const { return last_; }

 private:
  friend class base::RefCountedThreadSafe<Target>;
  ~Target() = default;
  int last_{0};
};

// 带 WeakPtrFactory 的接收者：Factory 必须是最后一个成员（门禁规则 C19），
// 这样它最先析构，在对象其余部分销毁之前先把所有 WeakPtr 置失效。
class Receiver {
 public:
  Receiver() { LOG(INFO) << "[Receiver] 构造"; }
  ~Receiver() { LOG(INFO) << "[Receiver] 析构"; }
  void Ping(int n) const { LOG(INFO) << "    Receiver::Ping(" << n << ")"; }
  int Square(int n) const { return n * n; }
  base::WeakPtr<Receiver> GetWeakPtr() { return factory_.GetWeakPtr(); }

 private:
  base::WeakPtrFactory<Receiver> factory_{this};
};

// ===========================================================================
void Part1_OnceCallback() {
  LOG(INFO) << "========== 第 1 部分：OnceCallback（move-only，一次消费）==========";

  // (a) 默认是 null
  base::OnceClosure cb;
  LOG(INFO) << "(a) 默认 null：is_null=" << cb.is_null()
            << "  operator bool=" << static_cast<bool>(cb);

  // (b) 从 lambda 构造，Run 必须作用在右值上（std::move），调用后回调被消费
  int out = 0;
  base::OnceClosure set([&out]() { out = 7; });
  LOG(INFO) << "(b) Run 前非空：bool=" << static_cast<bool>(set);
  std::move(set).Run();
  LOG(INFO) << "    执行后 out=" << out
            << "；set 已被消费 is_null=" << set.is_null();

  // (c) 带返回值
  base::OnceCallback<int(int)> triple([](int v) { return v * 3; });
  LOG(INFO) << "(c) 返回值：triple.Run(5)=" << std::move(triple).Run(5);

  // (d) 先判空再 Run（Repeating 同理），避免对 null 调用触发 CHECK 失败
  base::OnceClosure maybe;
  if (maybe) {
    std::move(maybe).Run();
  } else {
    LOG(INFO) << "(d) maybe 为 null，安全跳过（直接 Run 会 CHECK 失败）";
  }

  // (e) Reset 清空
  base::OnceClosure r([]() {});
  r.Reset();
  LOG(INFO) << "(e) Reset 后 is_null=" << r.is_null();
}

void Part2_RepeatingCallback() {
  LOG(INFO) << "========== 第 2 部分：RepeatingCallback（可拷贝，多次执行）==========";

  // (a) 可 Run 多次，拷贝共享同一份绑定状态
  int total = 0;
  base::RepeatingCallback<void(int)> add =
      base::BindRepeating([](int* out, int v) { *out += v; }, &total);
  add.Run(1);
  add.Run(2);
  add.Run(3);
  LOG(INFO) << "(a) 多次 Run 累计 total=" << total;

  // (b) 拷贝后共享同一状态（设计如此）
  base::RepeatingCallback<void(int)> copy = add;
  copy.Run(4);
  add.Run(1);
  LOG(INFO) << "(b) 拷贝共享状态：total=" << total;

  // (c) 带返回值
  base::RepeatingCallback<int(int)> plus1 = base::BindRepeating(&FreeAdd, 1);
  LOG(INFO) << "(c) 返回值：plus1.Run(10)=" << plus1.Run(10);

  // (d) 右值 Run 会消费回调（调用后变 null）
  base::RepeatingCallback<int(int)> consume = base::BindRepeating(&FreeAdd, 10);
  LOG(INFO) << "(d) std::move(consume).Run(5)=" << std::move(consume).Run(5)
            << "  消费后 bool=" << static_cast<bool>(consume);
}

void Part3_BindOnce() {
  LOG(INFO) << "========== 第 3 部分：BindOnce（绑定自由函数 / lambda / 成员）==========";

  // (a) 自由函数：参数全绑定 -> 闭包无需参数
  base::OnceCallback<int()> sum = base::BindOnce(&FreeAdd, 20, 22);
  LOG(INFO) << "(a) 全绑定 FreeAdd(20,22)=" << std::move(sum).Run();

  // (b) 自由函数：部分预绑定，尾部参数留到 Run 时给
  base::OnceCallback<int(int)> add40 = base::BindOnce(&FreeAdd, 40);
  LOG(INFO) << "(b) 部分绑定 FreeAdd(40,?) Run(2)=" << std::move(add40).Run(2);

  // (c) lambda
  int seen = 0;
  base::OnceClosure from_lambda =
      base::BindOnce([](int* p, int v) { *p = v; }, &seen, 9);
  std::move(from_lambda).Run();
  LOG(INFO) << "(c) lambda 绑定参数 seen=" << seen;

  // (d) 成员函数 + 裸指针接收者（直接传 &obj，不转移所有权）
  //     注意：裸指针接收者运行时「等效于 Unretained」——也是传裸 C* 当 this、
  //     零保活、对生命周期无影响；区别只在意图表达，见 (e)。
  Plain plain;
  base::BindOnce(&Plain::Note, &plain, 11).Run();
  LOG(INFO) << "(d) 裸指针接收者 Plain::Note(11) -> plain.last=" << plain.last;

  // (e) 成员函数 + Unretained（语义上“生命周期由我负责”，不持有引用）
  //     Unretained(&obj) 本质：显式声明此回调不持有 obj 的任何引用，其生死由我
  //     （程序员）负责，Bind 系统不提供保护。UnretainedWrapper<T> 只存一个裸 T*
  //     （bind.h:37-43），unwrap 时原样返回（bind.h:123）—— 不增引用计数、不建
  //     WeakPtr、对生命周期零影响。它与 (d) 「运行时完全一样」（都传裸 C* 当
  //     this、零保活），区别只在意图表达：Unretained 把「这里没保活、危险」写在
  //     代码表面，而裸指针把这点藏起来，将来改成异步执行照样编译却变 use-after-free。
  //     何时用：能从结构上证明目标比回调活得久（绑 this 且回调只在其存活期内触发，
  //     或本例栈对象同步 Run）。何时绝不能用：回调会逃逸到目标可能已死处
  //     （task/timer/跨线程/延迟）—— 必须改用 (g) WeakPtr。
  base::BindOnce(&Plain::Note, base::Unretained(&plain), 3).Run();
  LOG(INFO) << "(e) Unretained 接收者 Plain::Note(3) -> plain.last=" << plain.last;

  // (f) 成员函数 + scoped_refptr 接收者：主动保活（与 (e) 相反）
  //     BindState 把绑定参数「按值」存入 std::tuple（bind.h:244），注释明说
  //     scoped_refptr/WeakPtr 接收者按值存储以让绑定对象保持存活（bind.h:247-249）。
  //     故 BindState 自己持有一份 scoped_refptr，与 t 共享同一引用计数：
  //       MakeRefCounted   -> 计数 1（t 持有）
  //       绑进 BindState   -> 计数 2（按值存了一份）
  //       t 离开作用域     -> 计数 1，但 BindState 还持有一份 -> Target 不析构
  //       note_cb.Run()    -> Target 活着，安全（unwrap 取 p.get() 当 this，bind.h:132）
  //       note_cb 被消费   -> BindState 析构 -> 引用释放 -> 计数 0 -> Target 才 delete
  //     对比：(e) 零保活，目标可能已死时是你的 bug；(f) 钉住目标让它真执行；(g) 零
  //     保活但有 WeakPtr 守卫，目标死了自动变 no-op。何时用：想把引用计数对象绑进
  //     延后/异步回调且「确实要用到它」（如把一帧 VideoFrame 绑进解码完成回调）。
  //     代价：目标生命周期被回调延长，会推迟析构。前提：目标本身得是引用计数类型
  //     （RefCounted/RefCountedThreadSafe），故用 MakeRefCounted<Target>() 而非 Plain。
  base::OnceClosure note_cb;
  {
    auto t = base::MakeRefCounted<Target>();
    note_cb = base::BindOnce(&Target::Note, t, 42);
    // t 在此离开作用域，但 note_cb 内部仍持有一份引用 -> Target 仍存活
  }
  std::move(note_cb).Run();  // 安全：Target 还没析构
  LOG(INFO) << "(f) scoped_refptr 接收者让目标活过局部作用域（Target 析构在其后发生）";

  // (g) 成员函数 + WeakPtr 接收者：目标析构后回调自动变 no-op（核心安全机制）
  base::OnceClosure inert;
  {
    Receiver alive;
    base::BindOnce(&Receiver::Ping, alive.GetWeakPtr(), 1).Run();  // 存活 -> 执行
    inert = base::BindOnce(&Receiver::Ping, alive.GetWeakPtr(), 2);  // 延迟执行
  }  // alive 析构：WeakPtrFactory 先把 WeakPtr 失效
  LOG(INFO) << "(g) 目标已死再 Run 绑定的回调 ->";
  std::move(inert).Run();  // 检测到 WeakPtr 失效 -> 整段 no-op，不崩溃
  LOG(INFO) << "    （上面不会打印 Ping(2)；返回非 void 时会返回默认值）";

  // (h) 值类型预绑定参数在 Once 调用时完整到达
  std::string observed;
  base::OnceCallback<void(std::string*)> take =
      base::BindOnce(&TakeString, std::string("hello"));
  std::move(take).Run(&observed);
  LOG(INFO) << "(h) 值类型绑定参数完整到达 observed=\"" << observed << "\"";
}

void Part4_BindRepeating() {
  LOG(INFO) << "========== 第 4 部分：BindRepeating（可重复，绑定值不被移动）==========";

  // (a) 多次执行
  int total = 0;
  auto add = base::BindRepeating([](int* out, int v) { *out += v; }, &total);
  add.Run(1);
  add.Run(2);
  LOG(INFO) << "(a) 多次 Run total=" << total;

  // (b) 值类型绑定参数在多次 Run 之间不被移动出去（否则第二次会收到空值）
  auto take = base::BindRepeating(&TakeString, std::string("hello"));
  std::string first, second;
  take.Run(&first);
  take.Run(&second);
  LOG(INFO) << "(b) 值类型存活于多次 Run：first=\"" << first
            << "\" second=\"" << second << "\"";

  // (c) 返回值
  auto plus1 = base::BindRepeating(&FreeAdd, 1);
  LOG(INFO) << "(c) plus1.Run(10)=" << plus1.Run(10);
}

void Part5_Helpers() {
  LOG(INFO) << "========== 第 5 部分：callback_helpers ==========";

  // (a) DoNothing 返回“可执行的空闭包”，不是 null —— 因此 std::move(cb).Run() 永远安全
  base::OnceClosure dn = base::DoNothing();
  LOG(INFO) << "(a) DoNothing is_null=" << dn.is_null() << "（可安全 Run）";
  std::move(dn).Run();

  // (b) DoNothingRepeating 同理，可重复
  base::RepeatingClosure dnr = base::DoNothingRepeating();
  dnr.Run();
  dnr.Run();
  LOG(INFO) << "(b) DoNothingRepeating 可重复 Run";

  // (c) ScopedClosureRunner：离开作用域时自动 Run（用于“必定通知完成”的保证）
  LOG(INFO) << "(c) ScopedClosureRunner 在作用域结束自动 Run：";
  {
    base::ScopedClosureRunner runner(
        base::BindOnce([]() { LOG(INFO) << "    闭包在作用域结束被调用"; }));
  }

  // (d) 显式 Run + Reset 后，离开作用域不再重复执行
  int count = 0;
  {
    base::ScopedClosureRunner runner(
        base::BindOnce([](int* c) { ++*c; }, &count));
    runner.Run();
    runner.Reset();
  }
  LOG(INFO) << "(d) 显式 Run+Reset 后 count=" << count
            << "（作用域结束未再执行）";
}

}  // namespace

int main() {
  LOG(INFO) << "########## base/functional 演示开始 ##########";
  Part1_OnceCallback();
  Part2_RepeatingCallback();
  Part3_BindOnce();
  Part4_BindRepeating();
  Part5_Helpers();
  LOG(INFO) << "########## base/functional 演示结束 ##########";
  return 0;
}

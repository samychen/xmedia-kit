# examples/base/CMakeLists.txt — 纯 base 子系统的教学/对照 demo。
#
# 约定（与 examples/CMakeLists.txt 顶部一致）：目录结构与源码树 base/ 对齐，
# examples/base/<subsystem>/<subsystem>_demo.cc 对应 base/<subsystem>。所有目标
# 只链 xmedia::base，不依赖 FFmpeg/SDL/GL。需要宿主能力的 demo 不在此处，见上层。

# base/memory 所有权与生命周期演示：纯 base，不依赖 FFmpeg/SDL/GL，常驻。
# 作为 SDK 易用性的对照样例（docs/10 §6），也用于教学。
add_executable(memory_demo memory/memory_demo.cc)
target_link_libraries(memory_demo PRIVATE xmedia::base)
xmedia_configure_target(memory_demo)

# base/functional 回调与绑定机制演示：纯 base，不依赖 FFmpeg/SDL/GL，常驻。
# 覆盖 OnceCallback / RepeatingCallback / BindOnce / BindRepeating /
# callback_helpers（DoNothing、ScopedClosureRunner）以及四种绑定接收者
# （裸指针、Unretained、scoped_refptr、WeakPtr）。同样用于 SDK 易用性对照。
add_executable(functional_demo functional/functional_demo.cc)
target_link_libraries(functional_demo PRIVATE xmedia::base)
xmedia_configure_target(functional_demo)

# base/task 任务投递与消息循环演示：纯 base，不依赖 FFmpeg/SDL/GL，常驻。
# 覆盖 TaskRunner（PostTask/PostDelayedTask）、SequencedTaskRunner（默认 runner、
# 顺序保证）、TaskQueue（Run/Quit、延迟排序、诊断/序列 API、Clear 与引用环 RULE）
# 以及 task_runner_util 的 PostTaskAndReplyWithResult。用 ManualClock 确定性驱动。
add_executable(task_demo task/task_demo.cc)
target_link_libraries(task_demo PRIVATE xmedia::base)
xmedia_configure_target(task_demo)

# base/threading 线程与线程名/优先级演示：纯 base，不依赖 FFmpeg/SDL/GL，常驻。
# 覆盖 Thread（构造/Start/Stop、提前投递、顺序与同线程、序列判定与诊断 API、
# 优雅关闭 vs Stop 丢弃 pending）、platform_thread（SetName/GetName、优先级）、
# Thread::Options 与三档优先级，以及“一个 Thread = 一条 sequence”这一
# SEQUENCE_CHECKER 的运行时基础。用 WaitableEvent 真实多线程同步。
add_executable(threading_demo threading/threading_demo.cc)
target_link_libraries(threading_demo PRIVATE xmedia::base)
xmedia_configure_target(threading_demo)

# base/synchronization 锁/条件变量/原子标志/原子计数器演示：纯 base，常驻。
# 覆盖 Lock / AutoLock / AutoUnlock、ConditionVariable（配 Lock 的生产者-消费者）、
# AtomicFlag（跨线程取消/中断 + Reset）、AtomicSequenceNumber（序列号/代际号），
# 并在 (b) 段逐方法讲解 lock.cc 的死锁顺序检查器（HeldStack / ObservedOrder /
# AssertAcquiredInOrder / RecordRelease）。复用
# WaitableEvent 做精确跨线程同步（不 sleep）。
add_executable(synchronization_demo synchronization/synchronization_demo.cc)
target_link_libraries(synchronization_demo PRIVATE xmedia::base)
xmedia_configure_target(synchronization_demo)

export var ZOOM_STEPS = [0.25, 0.5, 0.75, 1, 1.5, 2, 3];
/* 播放头视觉时钟：播放时位置由 CSS transform transition 在「合成层」插值
   推进，完全不依赖 JS rAF。Safari 里同源 iframe 与父页共享同一个主线程，
   音轨首次 play() 的解码器 / 输出设备初始化会同步阻塞主线程数百毫秒，
   父页 rAF 也会一起停 —— 这正是音效起点前游标突然顿一下的根因（录屏
   实测 00:01.37 前连续 13 帧、216ms 不动）。合成层动画由 compositor
   独立驱动，主线程被占满时游标仍匀速前进。
   phVisual 只用于时间码、场景标签、自动滚动这些低频文本更新。 */
export var phX = function (t) { return HEAD_W + t * tlPPS; };
/* 时间轴：HEAD_W = 轨道头宽（常量），tlPPS 等可变量在 shared/store。 */
export var HEAD_W = 150;

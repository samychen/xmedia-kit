# Frame Worker——agent 逐帧创作协议（2026-11-19 实测通过）

CodeBuddy/claude 作为 frame worker 为单帧写**定制 HTML 画面**（非模板）。
本文件是该模式的 prompt 协议与渲染协议——实测产出质量超过预置模板
（镂空描边字/反转强调拍/左对齐偏置构图）。

## 渲染协议（必须遵守，否则全黑）

html_bake 的 seek 时钟**不推进 CSS animation**（元素停在 opacity:0
初始态，实测全黑）。帧画面必须用 **GSAP paused 时间线协议**：

```html
<script>
window.__timelines = window.__timelines || {};
var tl = gsap.timeline({ paused: true });
tl.from("#el1", { opacity: 0, y: 40, duration: 0.8 }, 0.2);
// 所有 tween 挂同一条 paused 时间线，绝对秒定位
window.__timelines["frame-<名>"] = tl;
</script>
```

外部逐帧调 `tl.totalTime(t)` 精确 seek，元素状态由 GSAP 直接写样式。
包装契约页需内联 GSAP 4 + `window.seek(tMs)` 驱动注册表。

## Worker Prompt 包（发给 CodeBuddy/claude 的完整任务书）

1. **帧规格**：项目 message、这帧的任务（type/persuasion/beat）、画布
   尺寸/时长
2. **两色纪律**：ground/ink 色值——强调只用反转/放大/密度，永不第三色
3. **负面清单**：禁紫蓝 AI 渐变/bokeh/浏览器 chrome/卡片投影/无限循环
4. **渲染协议**：上文 GSAP paused 协议（强制）
5. **输出格式**：单个 HTML 片段（div + style + script），写到指定路径

## 设计要求示例（实测产出达标）

大字钩子（90px+ 主行 / 28px 注释行）、左对齐 + 视觉重心偏置（避免居中
对称的模板感）、一根细发丝线、留白慷慨、一个反转强调拍（如 2.6s ink
实块擦入反白）。

## 实测记录

- prompt：/tmp/frame-packet.md（GSAP 协议版）
- 产出：frame-hook-v2.html（4145 字节，GSAP paused timeline ✓）
- 渲染：frame-v2.mp4 抽帧 t=1.5s/2.96s——左对齐构图/镂空字/反转拍
  全部符合设计要求；t=0 黑场→逐元素入场→2.6s 反转强调（时间轴编排）

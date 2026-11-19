---
name: launch
description: "从一句话/产品 URL/粘贴文案产出产品发布/宣传视频——SaaS 推广、功能发布、产品演示、公司启动。用户想推广/发布/展示产品时用；商业 URL 的默认。网站 tour 也归这里。产出 = spec 骨架 + 分镜审阅 + 导出 MP4。 [产品发布, launch, promo, 宣传片]"
---

**出处**：结构对齐 heygen-com/hyperframes 的 `product-launch-video`
skill（Apache 2.0，`vendor/product-launch-video/SKILL.md`）——Step 0-6
的顺序、gate 思想与 options-not-questions 沿用；执行细节全部替换为
本仓库的确定性管线（spec → 契约页 → html_bake），不是 agent 逐帧
自由创作。

**三层**：Floor = `references/floor.md`（gate 检查的底线）；默认方向 =
`xmedia.py storyboard` 的 Arc 库（每帧 type/persuasion/beat）；学习 =
study.md（后续）。你（agent）是编排者：按 Order 执行、每步过 gate 才
继续。全部步骤自己做；用户的停点是 Step 0 与 Step 3。

Order: brief（stop 0）→ capture → design → storyboard（stop 3 审批）
→ tts → build → gate → look → reply。工作目录 =
`~/.xmedia/projects/<project>/`（Home 创建时已定）；spec = 其
`spec.json`，契约页 = `contract.html`。

## 1. Brief（stop 0）

- 已有 `spec.json` 且含 `storyboard` 段 → 读它，什么也不问（brief 已
  定）。
- 没有 → 用 `xmedia.py storyboard <arc> --message ... --audience ...
  --mood ...` 生成骨架，**先给三个方向选项**（不同 Arc 或不同
  message 措辞，推荐者在前附一句理由）让用户选；用户不在场（自主
  模式）→ 按消息推断 Arc，作为 heads-up 声明后继续。
- Gate：`spec.json` 存在且含 `storyboard.arc` 与非空 `scenes`。

## 2. Capture（可选）

- 用户给了产品 URL → `xmedia.py brand scan <url>` 读色板/字体/站点名
  （注入 spec 配色）；`xmedia.py reference <视频>` 读竞品节奏。
- 无 URL → 跳过（Home 的 message 就是全部素材）。
- Gate：跳过，或 brand/reference 产物文件存在。

## 3. Design（设计系统）

- 有品牌 → `xmedia.py brand apply <名> --spec <spec>`。
- 无品牌 → 保持 Arc 默认色板（storyboard.direction.two_color）。
- Gate：`spec.json` 每个 scene 有 bg；storyboard.direction 存在。

## 4. Storyboard 与文案（stop 3 审批）

- 读 spec 的 `storyboard.frames`（每帧 type/persuasion/beat）与
  `scenes`（占位文案）。
- **占位文案是你要写的**：按 message 与每帧的任务（type/persuasion/
  beat）把 `[slot]`/占位文本替换成真实文案——hook 要钩人、social_
  proof 要具体数字、cta 要行动词。给出三组备选文案方向（不同语气）
  让用户挑，或自主模式选一组并声明。
- **审批 gate**：呈现逐镜计划（分镜页签的真实帧）请用户批准或修改；
  自主模式 → 作为 heads-up 声明后继续。
- Gate：无 `[slot]`/占位残留；用户批准（或 heads-up 已声明）。

## 5. TTS 与音频

- `xmedia.py tts <spec>` 生成旁白（文案已定时）。
- Gate：audio 轨存在或用户明确静音。

## 6. Build（契约页）

- `xmedia.py compose <spec> -o contract.html`——hf_validate 0 errors
  是硬门。
- Gate：compose exit 0 且 0 errors。

## 6b. Frame Worker（agent 逐帧创作——可选升级）

默认 build 用预置模板填空。需要**定制画面**（超越模板的设计水平）时，
每帧派一个 frame worker（CodeBuddy/claude `-p`）：prompt 包 =
`references/frame-worker.md` 的协议（帧规格 + 两色纪律 + 负面清单 +
**GSAP paused 渲染协议**——CSS animation 不被 seek 时钟推进，实测
全黑）→ worker 写单帧定制 HTML → 组装进契约页（data-start/
data-duration 窗口）。实测产出：镂空描边字/反转强调拍/左对齐偏置
构图，质量超过预置模板。

## 7. Gate → look → reply

- **gate 脚本**：`python3 skills/launch/scripts/gate.py <项目目录>`——
  hf_validate 0 errors + storyboard 完整性 + 无占位残留 + 音轨存在
  （或显式静音）。FAIL 行格式对齐 HF：`FAIL <rule>: <what and where>`。
- **look**：`/api/sb-frames`（或 `xmedia.py render` 抽帧）自己看每镜
  真实帧——占位文字/黑块/溢出都要在回复里指出。
- **reply**：平实的话——成片说了什么、多长、哪些是占位、哪些默认被
  打破及原因、用户可以在分镜卡上内联改或对话继续说。**从不声称 gate
  或快照没展示的东西。**

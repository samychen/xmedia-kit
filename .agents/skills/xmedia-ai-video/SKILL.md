---
name: xmedia-ai-video
description: xmedia-kit AI 视频生成管线——agent 按 spec JSON 确定性生成 MP4（TTS 旁白、7 类场景模板、竖屏、混音、来源验源），并支持把当前项目做成 launch 发布视频。凡用户提到"生成视频 / 做一条视频 / 宣传短片 / 把文案变成视频 / TTS 旁白视频 / 竖屏短视频 / 烤视频 / 给项目做发布视频 / launch 视频"，即使没点名 xmedia-kit，也用本管线——不要让模型手写 HTML 或引入其他渲染方案。
---

# xmedia-kit AI 视频生成管线

把用户的视频需求转成 spec JSON，走确定性管线出 MP4（同 spec 两次生成
逐字节一致）。本仓库（xmedia-kit）是唯一真源：完整 schema、字段语义、
已知边界见 `docs/HF_COMPOSE.md`——与本技能冲突时以文档为准。

## 两种工作流

**A. 主题 → 视频**（默认）：用户给文案或主题，按下面管线直接生成。

**B. 项目 → Launch 发布视频**（对齐 brag 工作流）：用户想给刚做好的
项目/网站/产品做一条发布视频时：
1. 读项目本体（README、代码结构、关键特性）——不需要 URL 或截图
2. 写 `<name>-output/plan.md`：产品角度（一句话卖点）、语气 tone
   （用户指定或推断）、逐 beat 故事板（场景/文字/时长/动画，总时长
   15–25s）、BGM 计划
3. 把故事板翻译成 spec JSON（plan 是给用户确认的产物，spec 是执行）
4. 走下面管线；交付时额外写 `share-copy.md`（社交媒体分享文案，一段
   主文案 + 可选话题标签）
5. 产物统一放 `<name>-output/`（已存在则加时间戳后缀防覆盖）

不用：实拍素材剪辑、复杂自由动效。Lottie 叠加有 `--lottie` 支持
（叠加层垫在 HTML 元素之下）。

## 管线（六步，缺一步不算完成）

```bash
# 1. 写 spec JSON（可直接复制 docs/HF_COMPOSE.md 的 demo.json 示例）

# 2. TTS 旁白（可选；say 离线零依赖，或 OmniVoice 式 HTTP 服务）
python3 tools/hf_tts.py demo.json            # → demo.narrated.json + voice/

# 3. spec → 契约 HTML
python3 tools/hf_compose.py demo.narrated.json -o demo.html

# 4. 契约校验——0 errors 是硬门，失败先修 spec 重跑，不许跳过
python3 tools/hf_validate.py demo.html

# 5. Studio 预览页（可选）：浏览器打开 demo.studio.html 边看边调 spec
python3 tools/hf_studio.py demo.html

# 6. 烘焙（--seconds 取 hf_compose 打印的总时长；尺寸与 format 对齐；
#    烘焙用原契约页 demo.html，不用 studio 页）
./build/bin/html_bake.app/Contents/MacOS/html_bake \
  --url ./demo.html --out demo.mp4 \
  --seconds 6.2 --fps 30 --width 1080 --height 1920 --clock seek

# 7. 交付三件套：demo.mp4 + demo.html + demo.html_elements.json
#    （launch 工作流另有 plan.md + share-copy.md，见工作流 B）
```

## 硬性约定

- agent **不手写 HTML** 冒充管线产物——手写页（进阶场景）也必须过
  hf_validate，且没有生成器注释（来源不可证，见下）
- `--seconds` 取 hf_compose 打印的总时长（旁白拟合后 ≠ 手写 duration
  之和）
- `--width/--height` 与 `composition.format` 对齐：16:9→1920×1080、
  9:16→1080×1920、1:1→1080×1080、4:5→1080×1350
- 未知参数会显式告警——`--height` 别写成 `--hight`

## 交付前必做：来源验证（查产物，不采信口头声明）

三重锚点逐级可验（MP4 → HTML → spec），交付时把验证输出贴给用户：

```bash
head -2 demo.html                              # 生成器注释（含 spec sha256）
ffprobe -show_format demo.mp4 | grep comment   # MP4 metadata（含 html sha256）
grep generator demo.html_elements.json         # 工具指纹
```

任一锚点缺失 = 产物不是本管线生成，不得交付。

## spec 最小集

| 字段 | 说明 |
|---|---|
| `composition` | `{id, format, width, height, fps}`——format 预设覆盖 width/height |
| `scenes[].type` | `title`/`text`/`outro`/`quote`/`list`/`stats`/`comparison` |
| `scenes[].animation` | `fade_in`/`fade_in_up`/`fade_in_down`/`slide_in_left`/`slide_in_right`/`scale_in` |
| `scenes[].narration` | 旁白路径（hf_tts 回填）；场景时长 = 旁白实长 + padding |
| `scenes[].narration_volume` / `audio` | 旁白音量 / BGM `{src, volume, fade_in, fade_out}`；音频文件缺失 → 输出无音轨并提示 |

专有字段（quote/author、items、value/description、left/right_items）见
`docs/HF_COMPOSE.md`。TTS 可朗读文本 = `text` 优先、按类型聚合专有字段
（stats 取 value/description 等——专有类型不因无 text 漏旁白）。

## 音频与旁白编排

- TTS 可朗读文本 = `text` 优先、类型聚合兜底；想口播版与屏上文字不同
  （旁白补画面而非读字），手写 `narration` 音频路径跳过自动提取
- BGM/SFX：引擎支持任意音频文件多轨叠加（`audio` 段 + 逐场景 narration
  按 data-start 混音）。SFX 内置合成素材（assets/sfx/，三级自动选择：
  场景 sfx 覆盖 → 关键词 → 类型默认，`--no-sfx` 关闭）；BGM 用用户
  提供的文件，缺失会明确提示且输出无音轨
- 旁白时长驱动场景时长（拟合 + padding）——写旁白文案时按节奏预算

## 常见坑

- 改了 spec 重跑：hf_tts 幂等（text 变了会重新生成对应场景旁白）
- `--chunks N>1`：音频块边界 ±1 AAC 帧（≈21ms）相移；采样级精确用
  `--chunks 1`；拼接产物不保留 MP4 metadata——验源用 HTML/sidecar 锚点
- 测试样例：`tests/testdata/tts/`（固定旁白 wav + 混合类型 spec）；
  回归 `ctest -R hf_tts_smoke`

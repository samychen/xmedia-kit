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

**C. 实拍素材 → 成片**（对齐 `Make videos agentically` 演示）：用户说「导入
刚录的视频、排到时间轴、加字幕、加片尾卡」时：
1. `python3 tools/xmedia.py import <dir> -o <name>-output/spec.json --asr --endcard`
   （ffprobe 实长自动排时间轴；`--asr` 逐段语音转写；无 whisper 时自动
   降级到同目录 SRT/VTT；`--endcard` 追加 outro-brand 模板）
2. 检查产出的 spec（场景数 / duration / `captions.segments`），按需改文案
3. 走下面管线；Studio 里可用右下角 `Ask AI`（Ctrl/⌘ + K）继续用自然语言改

不用：复杂自由动效、逐词卡拉 OK 字幕。Lottie 叠加有 `--lottie` 支持
（叠加层垫在 HTML 元素之下）。

## 管线（统一 CLI 优先）

**一条命令全链**（TTS→compose→validate→bake，参数全自动推导；首次使用先
`xmedia doctor` 自检环境）：

```bash
python3 tools/xmedia.py doctor                        # 环境自检（8 项）
python3 tools/xmedia.py render demo.json --out demo.mp4
```

单步（需要中间检查/调整时；交付三件套与 launch 工作流的 plan.md/
share-copy.md 见工作流 B）：

```bash
# 1. 写 spec JSON（可直接复制 docs/HF_COMPOSE.md 的 demo.json 示例）

# 2. TTS 旁白（可选；say 离线零依赖，或 OmniVoice 式 HTTP 服务）
python3 tools/xmedia.py tts demo.json            # → demo.narrated.json + voice/

# 3. spec → 契约 HTML
python3 tools/xmedia.py compose demo.narrated.json -o demo.html

# 4. 契约校验——0 errors 是硬门，失败先修 spec 重跑，不许跳过
python3 tools/xmedia.py validate demo.html

# 5. Studio 预览/编排（可选）：注入模式给单文件预览；服务模式给完整
#    编排 UI（spec 编辑热重组 + TTS + 一键烘焙，浏览器打开 127.0.0.1:8120）
python3 tools/xmedia.py studio demo.html
python3 tools/xmedia.py studio demo.html --serve --spec demo.narrated.json --port 8120

# 6. 烘焙（--seconds 取 hf_compose 打印的总时长；尺寸与 format 对齐；
#    烘焙用原契约页 demo.html，不用 studio 页）
python3 tools/xmedia.py render demo.narrated.json --out demo.mp4 --no-tts
```

AI 能力子命令（详见 `docs/HF_COMPOSE.md`「AI 能力集成」）：

```bash
# 素材自动导入：实长排时间轴 + ASR 字幕 + 品牌片尾卡
python3 tools/xmedia.py import ~/Downloads -o spec.json --asr --endcard
# 语音转写 / SRT 导入 → captions.json（填进 spec.captions.segments）
python3 tools/xmedia.py asr clip.mp4 -o captions.json
python3 tools/xmedia.py asr clip.srt -o captions.json
# 自然语言指令自检（Studio 的 Ask AI / MCP 用同一引擎）
python3 tools/xmedia.py agent "加一个片尾卡" spec.json
# stdio MCP server（供 Claude/Cursor/Codex/WorkBuddy 调用）
python3 tools/xmedia.py mcp --studio-url http://127.0.0.1:8120
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
| `scenes[].video` | 实拍视频素材路径（相对 spec 目录或绝对路径），画面走烘焙侧原生视频层 |
| `captions` | `{enabled, segments:[{start,end,text}]}`——**全局绝对秒**分段字幕，由 `hf_asr.py` 产出；自动按场景窗口裁剪。场景级 `scenes[].captions` 用**场景相对秒** |

专有字段（quote/author、items、value/description、left/right_items）见
`docs/HF_COMPOSE.md`。TTS 可朗读文本 = `text` 优先、按类型聚合专有字段
（stats 取 value/description 等——专有类型不因无 text 漏旁白）。

## Agent / MCP / Ask AI

Agent **不手写 HTML**，按 schema 填 spec JSON；所有外部 spec 都要过
`hf_agent.validate_spec()` 硬门（场景类型、时长、禁止远程 URL）。

- Studio 右下角 `Ask AI`（Ctrl/⌘ + K）：自然语言 → `/api/agent` → 写回 spec
  → 重组 → 预览/时间轴同步刷新。本地规则引擎零配置可用；设置
  `XMEDIA_LLM_ENDPOINT`（OpenAI 兼容）+ `XMEDIA_LLM_API_KEY` 后支持自由指令，
  密钥只在服务端。
- `POST /api/recompose` / `/api/tts` / `/api/bake` / `/api/asr` / `/api/import`
  / `/api/agent` 是全部可编程接口；`GET /api/state` 读当前状态与校验结果。
- `xmedia mcp` 启动 stdio JSON-RPC server（配置见项目根
  `mcp.json.example`），11 个 tools 覆盖状态、重组、TTS、烘焙、导入、ASR、
  校验；MCP 客户端可直接调用，不依赖 Studio UI。
- ASR 真语音识别需要 whisper.cpp 或 faster-whisper；缺失时自动降级到同目录
  SRT/VTT，**不要因为降级就宣称「AI 自动识别了语音」**——检查返回里的
  `provider` 与 `fallback_from` 字段。

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

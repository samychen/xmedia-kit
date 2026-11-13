# HyperFrames AI 视频生成管线

Agent（或人类）填 spec JSON → 生成器出契约 HTML → html_bake 烤 MP4。
三步管线，同 spec 两次生成逐字节一致（确定性）。

返回：[README](../README.md) ｜ 相关：[EXAMPLES](EXAMPLES.md) ｜ [COOKBOOK](COOKBOOK.md)

## 快速上手

```bash
# 1. 写 spec JSON（agent 或人类填空）
cat > my_video.json <<'EOF'
{
  "composition": {"id": "demo", "width": 1920, "height": 1080, "fps": 30},
  "background": "#0a0a1a",
  "scenes": [
    {"type": "title", "text": "产品发布", "subtitle": "v2.0",
     "duration": 3, "bg": "#0a0a1e", "text_color": "#ffffff",
     "animation": "fade_in_up"},
    {"type": "text", "text": "核心特性",
     "duration": 4, "bg": "#16213e", "text_color": "#e0e0e0",
     "animation": "slide_in_left",
     "content": [{"text": "• 确定性渲染"}, {"text": "• 音频混音"}]},
    {"type": "outro", "text": "Thanks",
     "duration": 2, "bg": "#0f3460", "text_color": "#ffffff",
     "animation": "fade_in"}
  ],
  "audio": {"src": "bgm.mp3", "volume": 0.6, "fade_in": 1, "fade_out": 2}
}
EOF

# 2. 生成契约 HTML
python3 tools/hf_compose.py my_video.json -o my_video.html

# 3. 验证契约合规（0 errors 才交给 html_bake）
python3 tools/hf_validate.py my_video.html

# 4. 烘焙 MP4
./build/bin/html_bake.app/Contents/MacOS/html_bake \
  --url "file://$(pwd)/my_video.html" \
  --out my_video.mp4 \
  --seconds 9 --fps 30 \
  --width 1920 --height 1080 \
  --clock seek
```

### 旁白驱动 + 竖屏（AI-auto-generate-video 模式）

`hf_tts.py` 逐场景生成旁白并回填 `narration`，`hf_compose` 拟合时长并把
旁白发成 `<audio>` 元素进确定性混音；`format: "9:16"` 一键竖屏：

```bash
# 1. TTS 生成旁白（voice/scene-N.wav + 回填 narration → demo.narrated.json）
python3 tools/hf_tts.py demo.json

# 2. 拟合时长 + 生成契约 HTML
python3 tools/hf_compose.py demo.narrated.json -o demo.html

# 3. 验证 + 烘焙（9:16 → --width 1080 --height 1920）
python3 tools/hf_validate.py demo.html
./build/bin/html_bake.app/Contents/MacOS/html_bake \
  --url "file://$(pwd)/demo.html" --out demo.mp4 \
  --seconds 7.3 --fps 30 --width 1080 --height 1920 --clock seek
```

spec 侧也可以手写 `narration` 路径跳过 TTS（时长照样拟合）。场景时长 =
ffprobe(旁白) + padding，旁白 `<audio>` 与场景窗口同起点进混音，BGM
（默认 0.6 音量）衬在旁白之下。

## 功能速查（功能 → 命令）

| 功能 | 命令 / spec 字段 |
|---|---|
| 场景模板（7 类） | `hf_compose`，spec `scenes[].type`：`title`/`text`/`outro`/`quote`/`list`/`stats`/`comparison` |
| 入场动画（6 种） | `scenes[].animation`：`fade_in`/`fade_in_up`/`fade_in_down`/`slide_in_left`/`slide_in_right`/`scale_in` |
| 画幅预设 | `composition.format`：`16:9` / `9:16` / `1:1` / `4:5`（覆盖 width/height） |
| TTS 旁白 | `hf_tts.py <spec.json> [-o narrated.json] [--provider say\|http] [--voice NAME] [--rate N] [--endpoint URL] [--out-dir DIR]`；spec `tts` 段；环境变量 `TTS_PROVIDER`/`TTS_ENDPOINT`（兼容 `OMNIVOICE_ENDPOINT`）；幂等（产物可探测即跳过） |
| 旁白驱动时长拟合 | `scenes[].narration` + `padding`（场景时长 = ffprobe 实长 + padding，GSAP 游标与 clip 窗口同源）；旁白音量 `scenes[].narration_volume` |
| BGM | spec `audio: {src, volume, fade_in, fade_out}`（默认 0.6 音量衬在旁白之下） |
| 分块烘焙（逐场景幂等重渲） | `html_bake --chunks N`（每块独立编码器冷启 + Concat 无损拼接） |
| 烘焙时钟 | `--clock seek`（页面契约，帧级精确，默认推荐）/ `virtual_time`（VT 步进）/ `live`（墙钟） |
| 编码器 / 封装 | `--codec mjpeg`（默认，基础构建）/ `libx264`（x264 依赖层开启时）；输出封装按扩展名 `mp4`/`gif`/`webm`/`mov` |
| Lottie 叠加 | `html_bake --lottie <path> [--lottie-z N] [--lottie-speed S] [--lottie-loop N]`（z 垫在 HTML 元素之下，bake 时钟驱动） |
| 页面数据注入 | `html_bake --data '<json>'`（`window.update(d)` 契约，首帧后注入一次） |
| 嵌套合成 + 变量 | spec `content` 手写 `data-composition-src` / `data-variable-values`（注入 runtime 页内挂载，子合成经 `__hyperframes.getVariables()` 消费） |
| 元数据导出 | 自动产出 `<out>.html_elements.json`（元素时序/矩形，毫秒域；调试与合成工具消费） |
| 契约自查 | `hf_validate.py <html> [--json]`（composition 根/duration/clip/__timelines/GSAP/file:// 子资源告警——agent 无人值守的前置门） |
| HyperFrames 契约 | 生成器自动保证：`data-composition-id` 根 + clip `data-start/duration`(秒) + paused GSAP 注册 `window.__timelines` + 内联 GSAP（离线确定性）；驱动 = 注册表 `totalTime(秒+1e-3)` + 采集端 `data-time-unit="s"` 秒→毫秒归一 |
| 确定性音频混音 | 页面 `<audio>` 元素（data-start/duration/volume/fade-in/out）→ libav 解码 → AudioConverter 统一重采样 48k FLTP → media-start 修剪 → 增益/斜坡 → data-start 叠加 → AAC 192k |

### TTS provider（hf_tts.py）

| provider | 说明 | 依赖 |
|---|---|---|
| `say` | macOS 离线 TTS（`--data-format=LEI16@22050` 出 WAV）| 系统自带，零依赖 |
| `http` | 本地 TTS 服务：`POST {text, voice, rate}` → audio 字节（OmniVoice 式，对齐 AI-auto-generate-video 的本地服务架构）| 自备服务 |

```json
"tts": {
  "provider": "say",
  "voice": "Tingting",
  "rate": 200,
  "endpoint": "http://127.0.0.1:8123/tts",
  "out_dir": "voice"
}
```

CLI `--provider/--voice/--rate/--endpoint/--out-dir` 覆盖 spec；环境变量
`TTS_PROVIDER` / `TTS_ENDPOINT`（兼容 `OMNIVOICE_ENDPOINT）兜底。幂等：
产物已存在且 ffprobe 可探测则跳过，重复执行安全；无 `text` 或已有
`narration` 的场景跳过（保留既有旁白）。默认输出 `<stem>.narrated.json`。

## Spec Schema

### composition（合成参数）

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| id | string | `"main"` | 合成 ID（__timelines 注册键）|
| width / height | int | 1920 / 1080 | 画布尺寸（像素）|
| format | string | — | 画幅快捷预设：`16:9` / `9:16` / `1:1` / `4:5`，设置后覆盖 width/height |
| fps | int | 30 | 帧率 |

### background（全局背景）

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| background | string | `"#000000"` | 全局背景色（CSS 颜色）|

### scenes[]（场景数组，按序播放）

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| type | string | `"text"` | 场景类型：`title` / `text` / `outro` / `quote` / `list` / `stats` / `comparison` |
| text | string | `""` | 主文本 |
| subtitle | string | `""` | 副文本（仅 title 类型）|
| duration | float | 3 | 秒制时长（有 narration 时被拟合值覆盖，见下）|
| narration | string | — | 旁白音频路径（相对 spec 目录或绝对路径）；设置后场景时长 = ffprobe 音频时长 + padding |
| padding | float | 0.5 | narration 拟合时的尾部留白秒数 |
| narration_volume | float | 1.0 | 旁白混音音量（BGM 默认 0.6 衬在旁白之下）|
| bg | string | `"#000000"` | 场景背景色 |
| text_color | string | `"#ffffff"` | 文字颜色 |
| animation | string | `"fade_in"` | 入场动画（见下表）|
| content[] | array | `[]` | 子项（text/list 类型，每项 `{text}`）|

各场景类型的专有字段：

| 类型 | 专有字段 |
|---|---|
| title | `subtitle` |
| text | `content[]` |
| quote | `quote`（缺省用 `text`）、`author` |
| list | `items[]`（字符串数组，或用 `content[]`）|
| stats | `value`（大数字）、`description` |
| comparison | `left` / `right`（列标题）、`left_items[]` / `right_items[]` |

旁白驱动时长拟合：`narration` 优先于 `duration`——场景 clip 窗口与 GSAP 入场游标**同源拟合**，逐场景对齐旁白长度（AI-auto-generate-video 模式）。

### animation（入场动画预设）

| 名称 | 效果 |
|---|---|
| `fade_in` | 淡入 |
| `fade_in_up` | 淡入 + 上移 |
| `fade_in_down` | 淡入 + 下移 |
| `slide_in_left` | 左滑入 |
| `slide_in_right` | 右滑入 |
| `scale_in` | 缩放入 |

### audio（背景音乐/音效）

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| src | string | 必填 | 音频文件路径（相对 spec 目录或绝对路径）|
| volume | float | 0.6 | 音量（1 = 0dB，0 = 静音）|
| fade_in | float | 0 | 起点淡入秒数 |
| fade_out | float | 0 | 终点淡出秒数 |

## Agent 集成

AI agent（Claude / Cursor / Codex 等）的使用方式与 brag 一致——agent 不直接写 HTML，而是**按 schema 填 spec JSON**：

```
User: "做一个 10 秒的产品宣传片，深色背景，3 个场景"
Agent: 读本文档 schema → 填 spec JSON → python3 tools/hf_tts.py（旁白）
       → python3 tools/hf_compose.py → python3 tools/hf_validate.py
       （0 errors 才继续）→ html_bake 渲染 → 返回 MP4 路径
```

生成器保证契约合规：data-\* 秒制 + `__timelines` 注册 + GSAP 4 参数形 + 内联 GSAP（离线确定性，不依赖 CDN）。`hf_validate.py` 是 agent 的自查工具——0 errors 才交给 html_bake。

## 与 brag 的对应

| brag | xmedia-kit |
|---|---|
| agent 读项目代码 | agent 读本文档 schema |
| agent 写 HyperFrames HTML | agent 填 spec JSON → hf_compose.py 生成 |
| `npx hyperframes render` | `html_bake --url file://... --out out.mp4` |
| HyperFrames runtime 管理合成 | 注入 runtime + 注册表 seek（同款契约）|

区别：brag 的 LLM 直接写 HTML（灵活但可能格式出错）；xmedia-kit 的 spec→HTML 是**确定性模板引擎**（同 spec 必出同 HTML，schema 校验挡住格式错误）——更适合无人值守的 agent 流水线。

## 嵌套合成与变量（进阶）

生成的 HTML 支持 HyperFrames 原版嵌套/变量——在 spec 的 `content` 里手写 `data-composition-src` 即可嵌套子合成：

```json
{"type": "text", "text": "", "duration": 5,
 "content": [{"text": "<div data-composition-src='./sub.html' data-variable-values='{\"title\":\"Pro\"}'></div>"}]}
```

子合成内通过 `window.__hyperframes?.getVariables?.()` 消费变量。

## 已知边界

- `.clip` 生命周期：注入 runtime 接管 `.clip` 元素显隐（其揭示协议需官方管线握手）。无 `.clip` 标记的数据驱动页不受影响——**hf_compose.py 生成的 HTML 不使用 `.clip` 标记**（场景可见性由 data-start/duration + GSAP opacity 驱动）✓
- 时间单位：HF 页 `data-start/duration` 是秒，采集端自动归一到毫秒（根元素 `data-time-unit="s"` 声明，`__timelines` 注册表检测兜底）；消费端（合成/混音/元数据）统一毫秒，无需 CLI 单位开关。
- 音频轨：旁白 `<audio>` + BGM 进 html_bake 确定性混音（解码 → 修剪/增益/斜坡 → data-start 叠加，AudioConverter 统一重采样）✓。
- 分块音频：`--chunks N>1` 时音频按块独立 AAC 编码再拼接，块边界存在 ±1 AAC 帧（≈21ms）的相移——内容经段级比对一致；采样级精确请用 `--chunks 1`。
- 烘焙画布：`--width/--height` 必须与页面 CSS 尺寸一致（hf_compose 按 format 预设生成同尺寸页面）——不一致时元素 UV 窗口错位。输出分辨率固定 = CLI 尺寸，不随显示器 DPI 变化。

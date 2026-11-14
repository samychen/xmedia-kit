# xmedia-kit AI 视频生成管线

Agent（或人类）填 spec JSON → 生成器出契约 HTML → html_bake 烤 MP4。
三步管线，同 spec 两次生成逐字节一致（确定性）。

返回：[README](../README.md) ｜ 相关：[EXAMPLES](EXAMPLES.md) ｜ [COOKBOOK](COOKBOOK.md)

## 快速上手

**统一 CLI**（推荐，所有能力的入口；详细子命令 `xmedia --help`）：

```bash
python3 tools/xmedia.py init my-proj          # 脚手架：可运行示例 spec
python3 tools/xmedia.py render my-proj/demo.json --out my-proj/demo.mp4
                                             # 一条命令全链：TTS→compose→
                                             # validate→bake（参数全自动推导）
python3 tools/xmedia.py doctor               # 环境自检（8 项逐项报告）
```

单步命令（等价透传）：`xmedia tts/compose/validate/studio/sfx`。

手动分步（与统一 CLI 等价）：

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

# 4. 烘焙 MP4（--url 支持相对路径；--seconds 取 spec 总时长）
./build/bin/html_bake.app/Contents/MacOS/html_bake \
  --url ./my_video.html \
  --out my_video.mp4 \
  --seconds 9 --fps 30 \
  --width 1920 --height 1080 \
  --clock seek
```

### 旁白驱动 + 竖屏（AI-auto-generate-video 模式）

`hf_tts.py` 逐场景生成旁白并回填 `narration`，`hf_compose` 拟合时长并把
旁白发成 `<audio>` 元素进确定性混音；`format: "9:16"` 一键竖屏：

```bash
# 1. TTS 生成旁白（voice/scene-N-<文本哈希>.wav + 回填 narration →
#    demo.narrated.json；词变自动重生成，跨 spec 同目录不串音）
python3 tools/hf_tts.py demo.json

# 2. 拟合时长 + 生成契约 HTML
python3 tools/hf_compose.py demo.narrated.json -o demo.html

# 3. 验证 + Studio 预览（浏览器打开 demo.studio.html 边看边调）
python3 tools/hf_validate.py demo.html
python3 tools/hf_studio.py demo.html

# 4. 烘焙（9:16 → --width 1080 --height 1920）
./build/bin/html_bake.app/Contents/MacOS/html_bake \
  --url ./demo.html --out demo.mp4 \
  --seconds <总时长> --fps 30 --width 1080 --height 1920 --clock seek
```

可直接复制的 `demo.json`（旁白文本即场景文本，TTS 只认 `text`）：

```json
{
  "composition": {"id": "demo", "format": "9:16", "fps": 30},
  "background": "#0a0a1a",
  "tts": {"provider": "say", "voice": "Tingting", "rate": 200},
  "scenes": [
    {"type": "title", "text": "欢迎使用 xmedia-kit",
     "subtitle": "AI 视频生成管线", "bg": "#1a1a2e",
     "animation": "fade_in_up"},
    {"type": "stats", "value": "1026", "description": "回归测试全部通过",
     "bg": "#16213e", "animation": "scale_in"},
    {"type": "outro", "text": "谢谢观看", "duration": 2, "bg": "#0f3460"}
  ]
}
```

`--seconds` 取 `hf_compose` 打印的总时长（旁白拟合后可能 ≠ 手写 duration
之和）。spec 侧也可以手写 `narration` 路径跳过 TTS（时长照样拟合）。场景
时长 = ffprobe(旁白) + padding，旁白 `<audio>` 与场景窗口同起点进混音，
BGM（默认 0.6 音量）衬在旁白之下。

### xmedia Studio（本地服务，浏览器编排）

```bash
python3 tools/hf_studio.py demo-output/demo.html --serve \
  --spec demo-output/demo.narrated.json --port 8120
# 浏览器打开 http://127.0.0.1:8120
```

UI 闭环（对位 Remotion Studio 的编辑→预览→渲染）：左侧 spec 编辑器 →
**重组合成**（Ctrl/Cmd+Enter，热更新右侧预览 + 校验面板）→ **生成旁白**
（全场景 TTS）→ 场景时间轴点击跳转 → **烘焙 MP4**（尺寸/时长/fps 由 spec
自动推导，无需记 CLI 参数），产物与日志同面板。spec 文件即落盘产物——
重开服务或直接走 CLI 烘焙都接得上。

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
| 原版契约兼容 | 生成器自动保证：`data-composition-id` 根 + clip `data-start/duration`(秒) + paused GSAP 注册 `window.__timelines` + 内联 GSAP（离线确定性）；驱动 = 注册表 `totalTime(秒+1e-3)` + 采集端 `data-time-unit="s"` 秒→毫秒归一（契约对齐 heygen-com/hyperframes） |
| 确定性音频混音 | 页面 `<audio>` 元素（data-start/duration/volume/fade-in/out）→ libav 解码 → AudioConverter 统一重采样 48k FLTP → media-start 修剪 → 增益/斜坡 → data-start 叠加 → AAC 192k |
| SFX 音效 | 三级选择（场景 `sfx` 覆盖 → 文本关键词 → 类型默认）随场景起点叠加，默认音量 0.4；素材 = `assets/sfx/` 五个 ffmpeg 合成音效（whoosh/pop/ding/success/transition，零授权，生成命令见该目录 README）；关闭：`--no-sfx` 或 spec `"sfx": {"enabled": false}`；自定义目录 `sfx.dir`、音量 `sfx.volume` |
| Studio 预览 | 注入模式：`python3 tools/hf_studio.py out.html` → 注入传输条（播放/拖动/场景跳转/空格与方向键）的 `<stem>.studio.html`，浏览器打开即预览；幂等原地更新 |
| xmedia Studio 服务 | `python3 tools/hf_studio.py out.html --serve --spec spec.narrated.json [--port 8120]` → 浏览器 UI（127.0.0.1）：**spec 编辑器热重组**（Ctrl/Cmd+Enter）、场景时间轴点击跳转、TTS 一键、**烘焙按钮（尺寸/时长由 spec 自动推导）**、校验与日志面板——对位 Remotion Studio 的编辑→预览→渲染闭环 |

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

CLI `--provider/--voice/--rate/--endpoint/--out-dir` 覆盖 spec；优先级
CLI > spec > 环境变量（`TTS_PROVIDER` / `TTS_ENDPOINT`，兼容
`OMNIVOICE_ENDPOINT）> 默认。幂等：产物已存在且 ffprobe 可探测则跳过，
重复执行安全；可朗读文本 = `text` 优先，缺失时按类型聚合专有字段
（stats 取 value/description，quote 取 quote/author，list 取 items，
comparison 取列标题）；已有 `narration` 的场景跳过（保留既有旁白）。
默认输出 `<stem>.narrated.json`。

**交付产物**（与 narrated spec 同目录）：`<stem>.script.txt`——旁白全文
逐行（供字幕工具）；`<stem>.voice.m4a`——按场景起点定位拼接的独立旁白
轨（起点 = 旁白实长 + padding 累计，与 compose 拟合公式同源）。

测试样例与冒烟：`tests/testdata/tts/`（固定旁白 wav + spec）+
`tests/unit/hf_tts_smoke.py`（本地 HTTP mock 充当 TTS 服务，不依赖
say/网络）——`ctest -R hf_tts_smoke`。

## Spec Schema

### composition（合成参数）

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| id | string | `"main"` | 合成 ID（__timelines 注册键）|
| schema_version | string | `"1.0"` | spec 契约版本；与本构建支持版本不符时 compose 拒绝并提示（产物注释含 schema 锚点）|
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
| transition | string | — | 场景级转场覆盖（进入方式）：`fade` / `slide-left` / `slide-right` / `wipe` / `none`；全局默认走顶层 `transitions: {type, duration}`（默认 fade / 0.5s）。语义 = 进入侧提前 duration 进入前一场景尾窗，总时长随重叠收缩 |
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

## Agent 调用协议与来源验证

**调用协议**（agent 按序执行，缺一步不算完成）：

1. 读本文档 schema → 填 spec JSON（`demo.json` 示例见上）
2. `python3 tools/hf_tts.py spec.json`（旁白，可选）
3. `python3 tools/hf_compose.py spec.narrated.json -o out.html`
4. `python3 tools/hf_validate.py out.html`——**0 errors 是硬门**
5. `html_bake --url ./out.html --out out.mp4 --seconds <总时长> --fps 30 --width W --height H --clock seek`
6. 交付产物三件套：`out.mp4` + `out.html` + `out.html_elements.json`

**约定**：agent 不手写 HTML 冒充管线产物——手写页（进阶场景）也必须过
`hf_validate`，且没有生成器注释（来源不可证，见下）。

**来源验证**（查产物、不采信 agent 口头声明）——三重锚点逐级可验：

| 锚点 | 位置 | 验证命令 |
|---|---|---|
| spec 哈希 | `out.html` 头部生成器注释 | `head -2 out.html` |
| HTML 哈希 | `out.mp4` 容器 metadata comment | `ffprobe -show_format out.mp4 \| grep comment` |
| 工具指纹 | `out.html_elements.json` generator 字段 | `grep generator out.html_elements.json` |

链路：MP4 metadata（`xmedia-kit html_bake | html sha256:…`）→ HTML 头部
（`xmedia-kit hf_compose | scenes=N | spec sha256:…`，spec 哈希 =
canonical JSON 的 sha256）→ spec 源文件。产物任一锚点缺失 = 不是本管线
生成（注意：`--chunks N>1` 的拼接产物不保留 MP4 metadata——用 HTML 注释
与 sidecar 锚点验证）。

## 与 brag 的对应

| brag | xmedia-kit |
|---|---|
| agent 读项目代码 | agent 读本文档 schema |
| agent 写 HyperFrames HTML | agent 填 spec JSON → hf_compose.py 生成 |
| `npx hyperframes render` | `html_bake --url file://... --out out.mp4` |
| HyperFrames runtime 管理合成 | 注入 runtime + 注册表 seek（同款契约）|

区别：brag 的 LLM 直接写 HTML（灵活但可能格式出错）；xmedia-kit 的 spec→HTML 是**确定性模板引擎**（同 spec 必出同 HTML，schema 校验挡住格式错误）——更适合无人值守的 agent 流水线。

## 嵌套合成与变量（进阶）

生成的 HTML 兼容原版（heygen-com/hyperframes）嵌套/变量语义——在 spec 的 `content` 里手写 `data-composition-src` 即可嵌套子合成：

```json
{"type": "text", "text": "", "duration": 5,
 "content": [{"text": "<div data-composition-src='./sub.html' data-variable-values='{\"title\":\"Pro\"}'></div>"}]}
```

子合成内通过 `window.__hyperframes?.getVariables?.()` 消费变量。

## 已知边界

- `.clip` 生命周期：注入 runtime 接管 `.clip` 元素显隐（其揭示协议需官方管线握手）。无 `.clip` 标记的数据驱动页不受影响——**hf_compose.py 生成的 HTML 不使用 `.clip` 标记**（场景可见性由 data-start/duration + GSAP opacity 驱动）✓
- 时间单位：HF 页 `data-start/duration` 是秒，采集端自动归一到毫秒（根元素 `data-time-unit="s"` 声明，`__timelines` 注册表检测兜底）；消费端（合成/混音/元数据）统一毫秒，无需 CLI 单位开关。
- 音频轨：旁白 `<audio>` + BGM 进 html_bake 确定性混音（解码 → 修剪/增益/斜坡 → data-start 叠加，AudioConverter 统一重采样）✓。音频文件缺失时打印 `missing — skipped`、输出**无音轨**（不建零包空轨）——文件放对位置或删掉 spec 的 `audio` 段即可。
- 分块音频：`--chunks N>1` 时音频按块独立 AAC 编码再拼接，块边界存在 ±1 AAC 帧（≈21ms）的相移——内容经段级比对一致；采样级精确请用 `--chunks 1`。
- 烘焙画布：`--width/--height` 必须与页面 CSS 尺寸一致（hf_compose 按 format 预设生成同尺寸页面）——不一致时元素 UV 窗口错位。输出分辨率固定 = CLI 尺寸，不随显示器 DPI 变化。
- CLI 健壮性：`--url` 支持相对路径（自动补全 `file://` 绝对 URL）；未知参数显式告警（拼写错如 `--hight` 不再静默吞掉）；二进制带 `__RESTRICT` 段——外部 `DYLD_LIBRARY_PATH` 等环境变量不影响其 FFmpeg 加载（机器上共存同名旧库也安全）。

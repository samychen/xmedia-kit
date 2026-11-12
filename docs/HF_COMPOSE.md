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
  --clock seek --hf-seconds
```

## Spec Schema

### composition（合成参数）

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| id | string | `"main"` | 合成 ID（__timelines 注册键）|
| width / height | int | 1920 / 1080 | 画布尺寸（像素）|
| fps | int | 30 | 帧率 |

### background（全局背景）

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| background | string | `"#000000"` | 全局背景色（CSS 颜色）|

### scenes[]（场景数组，按序播放）

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| type | string | `"text"` | 场景类型：`title` / `text` / `outro` |
| text | string | `""` | 主文本 |
| subtitle | string | `""` | 副文本（仅 title 类型）|
| duration | float | 3 | 秒制时长 |
| bg | string | `"#000000"` | 场景背景色 |
| text_color | string | `"#ffffff"` | 文字颜色 |
| animation | string | `"fade_in"` | 入场动画（见下表）|
| content[] | array | `[]` | 列表项（仅 text 类型，每项 `{text}`）|

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
Agent: 读本文档 schema → 填 spec JSON → python3 tools/hf_compose.py
       → python3 tools/hf_validate.py（0 errors 才继续）
       → html_bake 渲染 → 返回 MP4 路径
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
- `--hf-seconds`：HF 的 data-duration 单位是秒；html_bake 内部是毫秒。烘焙 HF 契约页时传 `--hf-seconds` 做单位归一。
- 音频轨：当前 MC 的渲染器级音频接线待接；html_bake 的音频链路已验证（tone440.wav 端到端 ✓）。

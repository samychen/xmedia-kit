# CicadaPlayer 能力移植进度（PROGRESS）

> 活文档。状态标记：`[ ]` 未开始 · `[~]` 进行中（必须带 `since:` 时间戳）· `[x]` 完成（带证据）。
> 原计划书 PLAN.md（范围裁决 §0 / DoD §1 / 核销表 §1.5）已随文档精简移除，内容可经
> `git log --all -- docs/cicada-port/PLAN.md` 找回；本表自足记录剩余与完成。

**当前整体状态**：T1–T9 完成（2026-10-29）+ T8 三条延伸收口（UTC/追帧/DASH 静态，2026-10-31）
+ DASH 动态侧（live 重载窗口，2026-10-31）+ HyperFrames 对齐与 AI 视频生成管线全部完成
（2026-11-14，功能速查见下节）。CicadaPlayer 移植范围内**无未开题项**；
DASH 的 SegmentList/SegmentBase、多 Period、代表切换仍按 §"DASH 后续"登记。

## 剩余移植工作（PLAN §0 裁决为"部分"或"后置"的部分）

- [x] **T8a UTC 同步**（2026-10-31）：`hls::ParseIsoDateTimeMs`（自研 days-from-civil，Z/±HH[:]MM/闰日）+ `Segment::program_date_time_ms` 解析 EXT-X-PROGRAM-DATE-TIME；demuxer 经 `Host::OnUtcAnchor` 发布锚点（CompositeDemuxer 补转发）→ `Pipeline::Client` → `kUtcTime` 事件缓存 + `Player::GetUtcTimeMs()` 外推查询。判据 4 单测（解析 3 形状/闰日/拒绝；锚点事件 wire + 无锚拒绝）。多源时最后写入者赢（与主时间线=source 0 一致）。
- [x] **DASH demuxer 集成（静态 SegmentTemplate）**（2026-10-31）：`media/ffmpeg/dash_segment_demuxer.{h,cc,_open,_read}`（C1 三拆）。按 mime/带宽选 Representation（TS 复用 rep 一条链覆盖多类型；fMP4 init 段取一次、逐段前拼）；逐段 FFmpegDemuxer + 统一 SegmentTemplate 网格 rebase；跨段读无缝、seek 落目标段（多链 seek 栅栏）；（动态 MPD 当时结构化拒绝，已由下一条的 live 重载窗口接上）。`.mpd` URI 进 factory。判据 4 e2e（打开暴露流/跨段单时间线/seek/动态拒绝）。
- [x] **DASH 动态侧：live 重载窗口**（2026-10-31）：`mpd_parser` 解析 `availabilityStartTime`/`publishTime`/`minimumUpdatePeriod`/`timeShiftBufferDepth`，动态 manifest 由 `(publishTime − availabilityStartTime)` 落在段网格上定位窗口（不再拒绝）；`dash_segment_demuxer_live.{h,cc}` 按最小更新周期（钳在 0.5–30 s）重取 MPD，**按 URL 尾部对齐就地延长**各链而不重建（重建会让游标下的索引重编号、时间线跳变），并按 rep id 查找以免重载时静默换档。直播链尾**挂起读而不发 EOS**（发 EOS 会让最后一次 manifest 的地平线成为流终点），重载补到新段后重发。判据：2 条解析单测（窗口展开 11 段/无锚点结构化拒绝）+ 1 条 e2e（本地 HTTP 服务发布 2 段→重发 4 段，新段可播）。
- [ ] **DASH 后续**（登记不开题）：多 Period、SegmentList/SegmentBase、代表切换（对齐 HLS 的 ABR 半边）、UTCTiming 时钟同步。
- [x] **T8b 直播追帧**：`media/filters/live_edge_policy.*` 已落地（config.net.live_max_latency 驱动），2026-10-31 核对补记。
- 不移植（范围裁决，勿重开题）：Android/iOS/Flutter 平台栈、MediaCodec 原生集成、HDR10(AVFoundation)、插件机制/DCA。

## AI 能力功能速查（HyperFrames 对齐 + AI 视频生成管线，2026-11 全部完成）

> 逐条实施记录随完成删除，细节见 [HF_COMPOSE.md](../HF_COMPOSE.md)（spec schema / agent 集成 /
> 已知边界）、提交历史 `git log --oneline 79ac162..30fbf2d` 与下方「完成记录」。

### 管线总览（spec → MP4，同 spec 两次生成逐字节一致）

```bash
python3 tools/hf_tts.py demo.json                                  # 1. TTS 旁白（可选）
python3 tools/hf_compose.py demo.narrated.json -o demo.html        # 2. spec → 契约 HTML
python3 tools/hf_validate.py demo.html                             # 3. 契约校验（0 errors 才继续）
./build/bin/html_bake.app/Contents/MacOS/html_bake \
  --url "file://$(pwd)/demo.html" --out demo.mp4 \
  --seconds 7.3 --fps 30 --width 1080 --height 1920 --clock seek   # 4. 烘焙
```

### 功能 → 命令

| 功能 | 命令 / spec 字段 |
|---|---|
| 场景模板（7 类） | `hf_compose`，spec `scenes[].type`：`title`/`text`/`outro`/`quote`/`list`/`stats`/`comparison` |
| 入场动画（6 种） | `scenes[].animation`：`fade_in`/`fade_in_up`/`fade_in_down`/`slide_in_left`/`slide_in_right`/`scale_in` |
| 画幅预设 | `composition.format`：`16:9` / `9:16` / `1:1` / `4:5`（覆盖 width/height） |
| TTS 旁白 | `hf_tts.py <spec.json> [-o narrated.json] [--provider say\|http] [--voice NAME] [--rate N] [--endpoint URL] [--out-dir DIR]`；spec `tts` 段；环境变量 `TTS_PROVIDER`/`TTS_ENDPOINT`（兼容 `OMNIVOICE_ENDPOINT`）；幂等（产物可探测即跳过） |
| 旁白驱动时长拟合 | `scenes[].narration` + `padding`（场景时长 = ffprobe 实长 + padding，GSAP 游标与 clip 窗口同源）；旁白进混音：`scenes[].narration_volume` |
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

- [ ] **.clip 生命周期（剩余唯一集成边界）**：注入 runtime 会接管 `.clip` 元素显隐（其 `__hf.seek` 驱动的是 runtime 构建期捕获的 capturedTimeline；注册表直驱移动 GSAP 状态但不触发 runtime 的 clip 揭示）——blank 模板/mb 页实测 Title/box 在数帧后隐藏。完整支持需逆向 runtime 的 build/capture 握手（`__hf.buildReady`/clipWindow 一族）并实现等价注入。无 .clip 标记的数据驱动页（当前已验证的页型）不受影响。

---

## 任务状态

- [x] **T1 自研 HTTP(S) 网络数据源**（media/curl + 单测）
- [x] **T2 缓存层**（slice 内存缓存 + 文件级边播边缓存装饰器）
- [x] **T3 HLS 播放列表 demuxer**（m3u8 parser / AES-128 / playlist demuxer）
- [x] **T4 ABR**（估计器 + 缓冲水位算法 + HLS 边界切换集成）
- [x] **T5 DRM/密钥抽象**（KeyProvider 接口 + ClearKey + 端到端解密测试）
- [x] **T6 外挂字幕**（SRT/VTT 解析 + Player API + ticker 发 cue）
- [x] **T7 画面控制**（kCrop/mirror 进 GlScene + Scene 主层持久模式 + Player 四件套 API）
- [x] **T8 直播增强 + DASH MPD 延伸**（mpd_parser SegmentTemplate 基线；完整 DASH demuxer 为登记后续项）
- [x] **T9 总验收**（888/888 + no-ffmpeg 665/665 + invariants 全过，核销记录见完成表）

## 完成记录

| 任务 | 完成时间 | 证据（构建/测试命令与结果、关键文件） |
|---|---|---|
| T1 自研 HTTP(S) 网络数据源 | 2026-10-29 00:35 | `media/curl/curl_data_source.{h,cc}`（独立 target `xmedia::curl`，`XMEDIA_ENABLE_CURL` 开关）；接线 `player/source_wrapping.{h,cc}`（自 player_impl_prepare.cc 按 C1 拆出）+ `NetConfig::prefer_curl`；测试基建 `tests/support/local_http_server.{h,cc}` + 9 个单测 `tests/unit/media_curl/`。`ctest build/cicada-port` **833/833 通过**；no-ffmpeg 构建不受影响；`check_invariants` 全过。提交：feature/cicada-port |
| T2 缓存层 | 2026-10-29 01:25 | `media/filters/slice_buffer_source.{h,cc}`（LRU 分片内存缓存）+ `media/filters/file_cache_data_source.{h,cc}`（边播边存文件缓存，按 URL fnv1a 键、重启续播、预算封顶、IO 失败降级直读）；接线 `WrapCache(WrapRetry(...))`，启用 PlayerConfig 闲置的 `data_source.*` 三字段。9 个单测（含 NetworkGoneBelow 磁盘/网络判别法）。`ctest` **842/842 通过**；invariants 全过。提交：feature/cicada-port |

| T5 DRM/密钥抽象 | 2026-10-29 03:30 | `media/base/key_provider.h` 接口 + `media/ffmpeg/clear_key_provider.*` 默认实现 + `Deps::key_provider` 注入 + HLS EXT-X-KEY 路由。testdata/hls/enc 为 openssl 加密的真实 TS 段。871/871 ctest 通过；invariants 全过。提交：feature/cicada-port |

| T6 外挂字幕 | 2026-10-29 03:55 | `media/filters/{srt,webvtt}_parser.*` + `external_text_track.*`（纯逻辑 7 单测）；`Player::{Add,Select,Set delay,Remove}ExternalSubtitle` + PlayerImpl 100ms ticker（`player_impl_subtitles.cc`）+ player 级 e2e。879/879 ctest 通过；invariants 全过。提交：feature/cicada-port |

| T7 画面控制 | 2026-10-29 04:15 | `LayerContentMode::kCrop` + `GlScene::SetLayerMirror` + `CoverSourceRect` 纯几何（4 单测）+ `Scene::SetMainLayerModes`（Present 时主层应用，host hook 仍可覆写）+ `Player::{SetRotateMode,SetMirrorMode,SetScaleMode,SetVideoBackgroundColor}`。883/883 ctest 通过；invariants 全过。提交：feature/cicada-port |

| T8 DASH 基线 | 2026-10-29 04:35 | `media/ffmpeg/mpd_parser.*`（自研 XML 子集 tokenizer + SegmentTemplate 展开 + ISO-8601 时长），5 单测。888/888 ctest 通过。提交：feature/cicada-port |
| T9 总验收 | 2026-10-29 04:40 | macos-sdl2+curl 888/888；no-ffmpeg 665/665；`check_invariants` 603 文件全过；`check_api_compat` 0 breaking。核销表见本文件「完成记录」|

## 交接备注

- 工作分支：`feature/cicada-port`（基于 main @ 8844f10）。
- 移植范围裁决（哪些不移植及原因）见本文档 §0 的「不移植」一行，勿在执行中重新开题。
  PLAN.md 已随文档精简移除，原文可经 `git log --all -- docs/cicada-port/PLAN.md` 找回。
- xmedia-kit 硬性风格：C++20、`-fno-exceptions -fno-rtti`、Google Style（.cc/.h、2 空格、80 列、
  license 头、`XMEDIA_*_EXPORT` 宏、命名 `CamelCase()`/`snake_case()`/`member_` 尾下划线）。
  提交前尽量跑 `clang-format`、`cpplint`、`tools/check_invariants.py`，至少保证目标构建通过。
- 构建入口：`cmake --build build/macos-sdl2`（既有 build 目录可复用）；测试 `ctest` 对应 preset。
- 无中断时按顺序推进；本文件是唯一进度真源，**每完成一个子步骤都要更新**。

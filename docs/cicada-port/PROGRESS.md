# CicadaPlayer 能力移植进度（PROGRESS）

> 活文档。状态标记：`[ ]` 未开始 · `[~]` 进行中（必须带 `since:` 时间戳）· `[x]` 完成（带证据）。
> 原计划书 PLAN.md（范围裁决 §0 / DoD §1 / 核销表 §1.5）已随文档精简移除，内容可经
> `git log --all -- docs/cicada-port/PLAN.md` 找回；本表自足记录剩余与完成。

**当前整体状态**：T1–T9 完成（2026-10-29）+ T8 三条延伸收口（UTC/追帧/DASH 静态，2026-10-31）；余 DASH 动态侧为登记后续。

## 剩余移植工作（PLAN §0 裁决为"部分"或"后置"的部分）

- [x] **T8a UTC 同步**（2026-10-31）：`hls::ParseIsoDateTimeMs`（自研 days-from-civil，Z/±HH[:]MM/闰日）+ `Segment::program_date_time_ms` 解析 EXT-X-PROGRAM-DATE-TIME；demuxer 经 `Host::OnUtcAnchor` 发布锚点（CompositeDemuxer 补转发）→ `Pipeline::Client` → `kUtcTime` 事件缓存 + `Player::GetUtcTimeMs()` 外推查询。判据 4 单测（解析 3 形状/闰日/拒绝；锚点事件 wire + 无锚拒绝）。多源时最后写入者赢（与主时间线=source 0 一致）。
- [x] **DASH demuxer 集成（静态 SegmentTemplate）**（2026-10-31）：`media/ffmpeg/dash_segment_demuxer.{h,cc,_open,_read}`（C1 三拆）。按 mime/带宽选 Representation（TS 复用 rep 一条链覆盖多类型；fMP4 init 段取一次、逐段前拼）；逐段 FFmpegDemuxer + 统一 SegmentTemplate 网格 rebase；跨段读无缝、seek 落目标段（多链 seek 栅栏）；动态 MPD 结构化拒绝（登记后续）。`.mpd` URI 进 factory。判据 4 e2e（打开暴露流/跨段单时间线/seek/动态拒绝）。
- [ ] **DASH 后续**（登记不 开题）：动态 MPD 重载窗口、多 Period、SegmentList/SegmentBase、代表切换（对齐 HLS 的 ABR 半边）、UTCTiming 时钟同步。
- [x] **T8b 直播追帧**：`media/filters/live_edge_policy.*` 已落地（config.net.live_max_latency 驱动），2026-10-31 核对补记。
- 不移植（范围裁决，勿重开题）：Android/iOS/Flutter 平台栈、MediaCodec 原生集成、HDR10(AVFoundation)、插件机制/DCA。

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
| T9 总验收 | 2026-10-29 04:40 | macos-sdl2+curl 888/888；no-ffmpeg 665/665；`check_invariants` 603 文件全过；`check_api_compat` 0 breaking。核销表见 PLAN.md §1.5 |

## 交接备注

- 工作分支：`feature/cicada-port`（基于 main @ 8844f10）。
- 移植范围裁决（哪些不移植及原因）见 PLAN.md §0，勿在执行中重新开题。
- xmedia-kit 硬性风格：C++20、`-fno-exceptions -fno-rtti`、Google Style（.cc/.h、2 空格、80 列、
  license 头、`XMEDIA_*_EXPORT` 宏、命名 `CamelCase()`/`snake_case()`/`member_` 尾下划线）。
  提交前尽量跑 `clang-format`、`cpplint`、`tools/check_invariants.py`，至少保证目标构建通过。
- 构建入口：`cmake --build build/macos-sdl2`（既有 build 目录可复用）；测试 `ctest` 对应 preset。
- 无中断时按顺序推进；本文件是唯一进度真源，**每完成一个子步骤都要更新**。

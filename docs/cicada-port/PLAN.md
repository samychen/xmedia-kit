# CicadaPlayer 能力移植计划（PLAN）

> 目标：将 CicadaPlayer 有而 avbase 缺失的能力，按 avbase 的架构风格（Chromium 分层、
> C++20、`-fno-exceptions`、Google Style）抽象补充进 avbase。
> 进度跟踪：同目录 [PROGRESS.md](PROGRESS.md)（活文档，每任务完成即更新，崩溃恢复以它为准）。
> 工作分支：`feature/cicada-port`。

## 0. 范围裁决

| CicadaPlayer 能力 | 移植策略 |
|---|---|
| 自研 curl 网络源（HTTP/1.1、keep-alive、自定义 header/DNS/proxy、重试） | **全量移植**（T1） |
| 内存分片缓存 + 边播边缓存 | **移植**（T2；remux-mp4 形态改为字节级文件缓存，见 T2 说明） |
| HLS（master/media playlist、AES-128、live、无缝切换、LL-HLS 标签） | **全量移植**（T3，自研 demuxer 挂 avbase DataSource 抽象） |
| DASH | **部分**：MPD 解析与 SegmentTemplate 落地为 T3 延伸（T8），完整 DASH 后置 |
| ABR（带宽估计 + 缓冲水位算法） | **移植**（T4，算法对齐 CicadaPlayer AbrBufferAlgoStrategy 思想） |
| DRM（WideVine/FairPlay） | **仅抽象**（T5）：KeyProvider 接口 + HLS ClearKey 落地；平台 CDM 留扩展点（对齐 avbase D8 决策） |
| 字幕（外挂字幕加载/选择/延迟 + 渲染） | **部分**（T6）：SRT/VTT 解析 + 外挂轨道加载 API + cue 调度；渲染交宿主（avbase 文本腿既有决策） |
| 直播追帧 / UTC 同步 | **部分**（T8）：live edge 追帧配置 + UTC 事件（依赖 T3 live） |
| 旋转/镜像/缩放/背景色 | **移植**（T7，player API + GL sink 实现） |
| Android/iOS/Flutter 平台栈、MediaCodec 原生集成、HDR10(AVFoundation) | **不移植**（本机无法构建/验证目标平台；以文档记录扩展点） |
| 插件机制（prototype 注册）/ DCA | **不移植**：与 avbase 编译期装配（DecoderSelector）冲突；以 DataSource 装饰器链达成同等扩展性（T1/T2 即实例） |

## 1. 任务清单（按依赖排序，每项含 DoD）

### T1 自研 HTTP(S) 网络数据源
- 新增 `media/net/`：`http_data_source.{h,cc}`（实现 `media::DataSource`，libcurl easy，
  blocking read + abortable、`CURLOPT_RESOLVE` 自定义 DNS、IPv4/6 强制、自定义 header/
  referer/UA/proxy、超时与重试）、`http_data_source_descriptor` 扩展（URL 源描述）。
- `cmake/FindCurl.cmake`（照 FindSDL2 模式）+ `AvbaseOptions.cmake` 开关 `AVBASE_WITH_CURL`。
- DoD：单测覆盖（本地 HTTP fixture：200/206 Range/404/断流重试/Abort/自定义头验证）；
  保持 no-ffmpeg 配置可关。

### T2 缓存层（内存分片 + 边播边缓存）
- 新增 `media/cache/`：`slice_buffer_source.{h,cc}`（分片内存池缓存任意区间，`readAt` 随机读，
  对齐 CicadaPlayer `data_source/cache`）、`file_cache_data_source.{h,cc}`（边播边存文件级
  byte cache：下载前瞻写文件、读命中直接回内存/文件，seek 超出已缓存区间才回源）。
- 作为 `DataSource` 装饰器实现（wrap 任意上游源），player config 增加缓存配置项。
- DoD：单测（命中/回源/预取/容量淘汰/seek 行为）；T1 关闭时可用 MemoryDataSource 测试。

### T3 HLS 播放列表 demuxer（最大任务，可拆段提交）
- `media/hls/`：
  - `m3u8_parser.{h,cc}`：master + media playlist、EXT-X-KEY/BYTERANGE/DISCONTINUITY、
    LL-HLS 标签（EXT-X-PART/PART-INF/PRELOAD-HINT）解析；
  - `hls_stream.{h,cc}`：variant 流管理、segment 下载（经 T1/DataSource 抽象）、
    playlist live reload；
  - `hls_demuxer.{h,cc}`：实现 `media::Demuxer`，内嵌 per-segment FFmpegDemuxer 解 TS/fMP4 段，
    时间戳跨段连续化（rebase），variant 无缝切换（sequence 对齐）；
  - `media/crypto/aes_128.{h,cc}`：AES-128-CBC 解密（自研，过 NIST 测试向量），供 EXT-X-KEY。
- DoD：单测（parser 全标签、AES 向量、segment 装载与 rebase、live reload、切换对齐）；
  `examples/headless` 可播本地 m3u8。

### T4 ABR 自适应码率
- `media/abr/`：`bandwidth_estimator.{h,cc}`（EWMA 下载速度）、`abr_algorithm.{h,cc}`
  （缓冲水位 + 带宽双因子，接口可替换）、与 T3 `hls_stream` 的 variant 切换集成。
- DoD：单测（估计器收敛、切换迟滞）；策略接口可注入自定义实现。

### T5 DRM/密钥抽象
- `media/drm/`：`key_provider.{h,cc}`（按 key URI 取密钥的接口 + 回调注入）、
  `clear_key_provider.{h,cc}`（HLS AES-128 key 下载落地）、`cdm_context.{h}`（平台 CDM
  扩展点，WideVine/FairPlay 留白，文档记录 CicadaPlayer 对应物）。
- DoD：T3 的 EXT-X-KEY 走 KeyProvider；单测（provider 注入/失败路径）。

### T6 外挂字幕
- `media/text/`：`srt_parser.{h,cc}`、`webvtt_parser.{h,cc}`（复用 `media::TimedText` 类型）、
  `external_text_loader.{h,cc}`（从 DataSource/URL 加载文本轨）。
- player API：`AddExternalSubtitle(descriptor, delay)` + 选择/延迟设置；cue 经既有文本腿回调。
- DoD：单测（SRT/VTT 解析边界、延迟对齐）；headless 示例演示 cue 输出。

### T7 画面控制（旋转/镜像/缩放）
- `player/public/player.h` 增 `SetRotateMode/SetMirrorMode/SetScaleMode/SetVideoBackgroundColor`；
  GL sink（render/gl）实现变换矩阵注入；SDL2 后端经纹理变换。
- DoD：单测（矩阵生成纯函数）；示例可切换模式。

### T8 直播增强 + DASH MPD 延伸
- live edge 追帧配置（`live_edge_chase`、`max_live_delay`）接入时钟决策；UTC 时间事件下发；
  `media/dash/mpd_parser.{h,cc}`（SegmentTemplate 基线，文档标注后置范围）。
- DoD：单测 + 文档。

### T9 总验收
- 全 preset 构建 + 全测试 + `check_invariants.py` + clang-format/cpplint；
  README/docs 登记（docs/PROGRESS.md 与新模块指南）；对照本文档 §0 逐项核销，输出结论。

## 2. 执行与恢复协议

1. 顺序执行 T1→T9；每任务完成：构建 + 单测通过 → 更新 PROGRESS.md（勾选 + 证据）→ 下一任务。
2. 任何会话（含 30 分钟监督自动化）开工前：读 PROGRESS.md 找第一个未完成任务；若某任务标记
   `IN_PROGRESS` 且 `since` 距今 < 90 分钟，视为他人进行中，只报告不抢；否则接管并更新 `since`。
3. 平台不可移植项不阻塞验收；以扩展点 + 文档交付。

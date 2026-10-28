# CicadaPlayer 能力移植进度（PROGRESS）

> 活文档。恢复执行协议见 [PLAN.md](PLAN.md) §2：接手者读本表，从第一个未完成任务继续。
> 状态标记：`[ ]` 未开始 · `[~]` 进行中（必须带 `since:` 时间戳）· `[x]` 完成（带证据）。

**当前整体状态**：T1 完成，T2 进行中。

---

## 任务状态

- [x] **T1 自研 HTTP(S) 网络数据源**（media/net + FindCurl + 单测）
- [~] **T2 缓存层**（media/cache：slice 内存缓存 + 文件级边播边缓存装饰器） since: 2026-10-29 00:40
- [ ] **T2 缓存层**（media/cache：slice 内存缓存 + 文件级边播边缓存装饰器）
- [ ] **T3 HLS 播放列表 demuxer**（media/hls：m3u8 parser / AES-128 / hls_stream / hls_demuxer）
- [ ] **T4 ABR**（media/abr：带宽估计 + 缓冲水位算法 + 与 HLS 集成）
- [ ] **T5 DRM/密钥抽象**（media/drm：KeyProvider + ClearKey + CDM 扩展点）
- [ ] **T6 外挂字幕**（media/text：SRT/VTT 解析 + 加载 API + cue 调度）
- [ ] **T7 画面控制**（rotate/mirror/scale：player API + GL sink）
- [ ] **T8 直播增强 + DASH MPD 延伸**（追帧配置 + UTC 事件 + mpd_parser）
- [ ] **T9 总验收**（全构建/测试/invariants/文档登记/对照 PLAN §0 核销）

## 完成记录

| 任务 | 完成时间 | 证据（构建/测试命令与结果、关键文件） |
|---|---|---|
| T1 自研 HTTP(S) 网络数据源 | 2026-10-29 00:35 | `media/curl/curl_data_source.{h,cc}`（独立 target `avbase::curl`，`AVBASE_ENABLE_CURL` 开关）；接线 `player/source_wrapping.{h,cc}`（自 player_impl_prepare.cc 按 C1 拆出）+ `NetConfig::prefer_curl`；测试基建 `tests/support/local_http_server.{h,cc}` + 9 个单测 `tests/unit/media_curl/`。`ctest build/cicada-port` **833/833 通过**；no-ffmpeg 构建不受影响；`check_invariants` 全过。提交：feature/cicada-port |

## 交接备注

- 工作分支：`feature/cicada-port`（基于 main @ 8844f10）。
- 移植范围裁决（哪些不移植及原因）见 PLAN.md §0，勿在执行中重新开题。
- avbase 硬性风格：C++20、`-fno-exceptions -fno-rtti`、Google Style（.cc/.h、2 空格、80 列、
  license 头、`AVBASE_*_EXPORT` 宏、命名 `CamelCase()`/`snake_case()`/`member_` 尾下划线）。
  提交前尽量跑 `clang-format`、`cpplint`、`tools/check_invariants.py`，至少保证目标构建通过。
- 构建入口：`cmake --build build/macos-sdl2`（既有 build 目录可复用）；测试 `ctest` 对应 preset。
- 无中断时按顺序推进；本文件是唯一进度真源，**每完成一个子步骤都要更新**。

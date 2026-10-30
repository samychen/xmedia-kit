# 实施进度（活文档）

> 设计文档：[README](../README.md) ｜ 里程碑定义：[08 实施路线图](08-实施路线图与风险.md)
> 剩余工作权威清单：[12-剩余工作清单](12-剩余工作清单.md) ｜ 产品化能力 / 死选项 / 环境项：[TODO](TODO.md)
> CI 不可见时的等价门禁信号：[tools/gate_local.sh](tools/gate_local.sh)

> ⚠️ 本文档只保留**当前状态**与**已完成/未做速览**。过程性记录已移除——
> 属历史审计轨，不进活文档；如需追溯见 `git log` 与旧版 `PROGRESS.md` 的 git 历史。
>
> 早期的记录在 [archive/PROGRESS-rounds-1-9](archive/PROGRESS-rounds-1-9.md)。
> 代码注释里写的 `PROGRESS.md §3l`、`§(5)` 等旧章节号指向的是**那里**——本文档精简后
> 已没有这些小节（约 40 处注释仍用旧写法，按此转发阅读即可）。

---

## 当前状态

**M0–M11 ✅ · Phase 0/1/2/4 ✅（多数）· Phase 3 解码侧 ✅、显示侧 GPU 上屏 ✅（非零拷贝）、真零拷贝 ⬜ · Phase 5 转码代码全落地 + 编码链路实测（硬件加速本体待其他机器）+ `avbase-inspect transcode` 生产入口 ✅ · macOS 窗口播放器 ✅ · Qt 播放器骨架 ✅（时间线改为真实 `MediaInfo`）· Qt 门面已库化 ✅ · Qt 目标已进 CI ✅ · CI 风格门禁全绿 · 管线原生多源 ✅ · M15 C ABI ✅ · M13 SDK 文档七份 + apidocs 门禁 + 微基准 10 项 + e2e/渲染 CPU 机器判据 + 新鲜度门禁（比库也比源码）✅（剩 golden 基线录制、conan/vcpkg 实跑、soak 48h，均环境阻塞）**

最后更新：**2026-10-31**

---

## 已完成里程碑速览

- **M0–M6**：工程基建 / `base` 核心件 / `base` 任务系统 / `media` 值类型 / FFmpeg 解复用 / 解码流与 FFmpeg 解码器 / 音视频同步与合成器 —— 全部 ✅
- **M7 播放渲染**：`VideoRendererImpl` / `AudioRendererImpl` / `RendererImpl` / `TextRenderer` / 音频管线 —— ✅
- **M8 管线与播放器**：`pipeline_impl` / `player_impl` / `state_machine` / `event_hub` / `seek` / `buffer_controller` / `diagnostics` —— ✅（SDK 头冻结，方法除转码外全部落地）
- **M9 缓冲与 seek**：三级 HWM / 精确 seek / `RetryDataSource` / `LiveDataSource` / `UrlRewriteInterceptor` —— ✅
- **M10 验收基座**：`platform/null` / `synthetic_demuxer` / golden 基线工具 —— ✅
- **M11 窗口播放器**：macOS 窗口播放器（SDL2）✅；Linux 原生播放器（`play_native` / `play_embed`）✅
- **M12 Linux 平台层**：`gl` / `window` / `audio` / `zero_copy` dmabuf / dlopen loader —— ✅（约 3800 行）
- **M15 C ABI**：`player/public/c/avbase_c.h` 真通用门面（60 导出函数 + 门禁）—— ✅
- **管线原生多源（docs/14）**：`CompositeDemuxer` 聚合 N 源 / 每源一对渲染腿 / 时钟多轴 —— ✅
- **CI 风格门禁**：`check_invariants` / `clang-format` / `cpplint` / `check_binary_freshness` 全绿；`macos-player` 已修 `buildPresets.targets` 截断陷阱（只编 `play_sdl2` 漏掉 `player_unittests`，CI 长期隐形红）。
- **选项面**：构建注册表内 **10 个 key 全部有下游读取者（死选项 0）**——4 个 buffer key 与 `nodisp`/`enable-accurate-seek` 都是先写读取者再注册回来的；`volume`（2026-10-26 补）把 ijk 的 0..100 换算进 `audio.startup_volume`，而该字段现在真的被 `PlayerImpl` 的构造函数读取（此前只有校验、无人读，见 §3）；仍未接线的只是 DRAFT `.inc` 里的 spec key，见 [TODO](TODO.md) §2。

---

## 仍在进行 / 未做

| 项 | 状态 | 说明 |
|---|---|---|
| Phase 3 显示侧 GPU 上屏 | ✅ | `apps/player_qt` 把 YUV→RGB 交给着色器并画进 Qt 的 FBO（`QQuickFramebufferObject` + `platform/qt/`）|
| Phase 3 真零拷贝 | ⬜ | 上一条只搬走了算术，1080p 每帧 3.1 MB 仍从 CPU 内存进显存（实测 854×480 下 GPU 4.2 ms vs 软件 2.9 ms）；要 Linux(VAAPI-EGL)/Windows(D3D11-GL) 进 sink + `examples/pull_frames` 在对应硬件跑通 |
| Phase 5 E5 硬件编码器实测 | 🔶 | 编码链路已实测（VideoToolbox：30 帧 → 30 包 → 解回 30 帧，`hardware_encoder_unittest.cc`，突变验过）；**硬件会话本体未实测**——本机 Intel Mac 建不起来（-12903，仅 `allow_sw=1` 的软件实现可用），需 Apple Silicon / Linux VAAPI 机器。为让硬件编码器可用，`FFmpegVideoEncoder::Params` 与 `VideoEncoderFactory::CreateEncoder` 新增私有选项通道（VT 的 `allow_sw`、VAAPI 的 device 都靠它），并经 `TranscodeParams::VideoCodecSpec::options` 贯通到转码路径——CLI 侧是 `avbase-inspect transcode --encopt allow_sw=1 --vcodec h264_videotoolbox`（实测产出真 H.264） |
| 推流 / RTMP publish | ✅ | `avbase-inspect publish <file> -o rtmp://… --format flv`（2026-10-28）。`TranscodeParams::output_format` 让无扩展名的 URL 也能定容器（缺了就失败并说明原因，不静默写空流）；`TranscodeParams::realtime` 按输入时间戳配速（3.029 s 源：实时 3.02 s / `--no-realtime` 0.05 s）。真协议验收 `tools/check_rtmp_publish.py` 用 FFmpeg 的 `rtmp_listen` 当服务端（不需要 MediaMTX），回环收到 2 流、时长一致，并断言两种配速模式确实不同 |
| 缩略图 / seek preview 批量取帧 | ✅ | `PreviewFrames()` + `avbase-inspect preview -o <dir> --start S --count N [--interval S]`（2026-10-28）。关键帧 seek 会落在目标之前，落盘前按时间戳丢弃早于目标的帧（`--interval 0.5` 得到 0/500/1000 ms，而不是 0/0/1000）|
| 录制（播放侧落盘） | ✅ | `PlayerRecorder`（v2，tap 挂点版）：tap 在管线解码流消费侧，copy 模式直写 EncodeMuxer，muxer 写收敛到专用录制线程（双腿并发写 av_interleaved_write_frame 实测死锁）。判据 `player_ffmpeg_unittests` 3 条，共享树下 30/30 压测。三个竞态坑已修（2026-10-31）：muxer 拒绝 post-header 加流 + 录制器挂起队列等双腿流头齐才写 header（否则输家流的 `priv_pts` 未初始化，首包 `frac_add` 除零）；`Start` 向当前播放位置重锚（音频算法队列可预取 3 s，中段挂载会让一条腿整条饿死、容器里留死轨）；首条 EOS 挂 500 ms 兜底收尾。**render-tap 完整形态**（音量/暂停语义进录制）仍为后续项 |
| Qt 门面 | ✅ | 可复用库 `player/qt/`（`avbase::qtfacade`）已抽出，`apps/player_qt/` 只剩 `main.cc` + `qml/` 并链接它；时间线为真实 `MediaInfo`，`qt_unittests`（`tests/unit/qt/` 8 条）改为链接库、突变验过；M15 C ABI 已作 UE/跨语言挂点 |
| Qt 目标进 CI | ✅ | 新增 `qt` preset + CI job（Ubuntu 24.04 的 Qt 6.4.2）+ `gate_local --jobs qt`：建 `avbase_player_qt` 与 `qt_unittests` 并跑 ctest（本机 625/625，含 8 条 `TimelineModelTest`）。判据经突变验证：摘掉 `gtest_discover_tests` 后门禁变红（不是静默绿）|
| soak 48h 长稳 | ⬜ | 脚本 `tools/run_soak.py` 已就绪，48h 未跑（沙箱禁读他进程 RSS） |
| Windows CI 验证 | ⬜ | job 已加（no-ffmpeg，不开 `/WX`），首跑待回答 |
| M13 发布 0.1.0 | 🔵 | 文档七份 + apidocs 门禁 ✅；V9 注释覆盖率 / V10 API 兼容 / 微基准 **8 项**（新增 `BM_DecoderBufferQueue_PushPop` / `BM_DecoderBuffer_MoveConstruct` / `BM_VideoFrame_RefCountChurn`，已进基线） / 端到端 CPU 判据 / golden diff 件 / cpack TGZ / vcpkg port 均已完成并入门禁。剩：golden **录制**基线（需 ijkplayer 原版）、依赖解码器的 4 项 `BM_*`（需开 FFmpeg 的 bench 变体）、`conan` 实跑（本机 pip 装不上，网络不可达）、`cpack -G DEB` 待 Linux 机、vcpkg 未提交 registry。**渲染 CPU 的机器判据不做**：`tools/bench_render.sh` 要 ffmpeg CLI 与真实 display，本机两者都没有 |
| M14–M18 | ⏳ | VAAPI+HDR / Android / iOS / `ijkio` 缓存（后置增量） |
| 产品化能力（Android/iOS/Web/Unity/UE/Flutter/…） | ❌ | 全部未做，见 [TODO](TODO.md) |
| HDR 检测 + tone mapping | ✅ | 检测按传输特性（PQ/HLG），`kHdrUnsupported` 在 `kPrepared` 前上报；**tone mapping 已真做**：`zscale=t=linear:npl= → tonemap=hable:desat=0.5 → zscale=t=bt709`，接在 `VideoConverter` 里并排在 libyuv/sws 之前。实测同一帧 naive 122 / tone mapped 235。三层判据，关掉转换时单元测试报 148vs148、e2e 报 avg luma 123.4 |
| `kRotationChanged` | ✅ | 与 HDR 同类（值在 `video_rotation_` 里、事件零生产者）。现在源声明旋转时才发；测的是「事件值与 `video_rotation()` 必须一致」。角度是顺时针（270 = 90 逆时针 matrix），已核对 `gl_layer_geometry.cc` 的 CLOCKWISE 表 |
| 读源录制 | ✅ | `avbase-inspect record`，copy 优先，`--duration` 有界；判据量输出时长（40 s 源配 `--duration 3` 必须约 3 s）。**播放侧取包录制未做** |
| `kDecoderOpened` / `kDecoderFallback` | ✅ | 通道 `DecoderStream → 子 renderer → RendererImpl → PipelineImpl → PlayerImpl → EventHub` 接通，另有四个查询。踩坑：WeakPtr 跨序列 CHECK 中止（改 Unretained）、参数顺序错位会把「打开」报成「回落」 |
| 转码链的像素正确性 | ✅ | 10-bit / NV12 源此前转码「成功」但画面损坏（muxer OK、时长对、退出码 0，SSIM 0.035）；`AvFrameToVideoFrame` 改为按源格式分派（I420 走逐平面快路径，其余走 `ff::VideoConverter`），修后 SSIM 0.883。两条测试钉像素值，突变验过 |
| 决策项 | — | LL-HLS 自研 vs 接受 ~3s、M12 原生后端是否启动、版权主体、golden 基线锁版本，见 [12-剩余工作清单](12-剩余工作清单.md) §6 |

> 完整开放项、降级项、死选项、环境项见 [12-剩余工作清单](12-剩余工作清单.md) 与 [TODO](TODO.md)；
> 已修关键缺陷（关闭竞态 / VOD 死锁 / 解码器丢包 / corpus 三崩溃 / EOF 后 seek 永不回调 /
> 暂停中 seek 画面跑飞 / 丢帧计数变成刷新率函数 / fps-cap 判错对象 · 丢帧原因说不出来 /
> 起播后画面定格 / 时间线面板假数据）记录在 [12-剩余工作清单](12-剩余工作清单.md) §3。

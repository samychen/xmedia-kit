# 实施进度（活文档）

> 设计文档：[README](../README.md) ｜ 产品化能力 / 死选项 / 环境项：[TODO](TODO.md)
> CI 不可见时的等价门禁信号：[tools/gate_local.sh](tools/gate_local.sh)

> ⚠️ 本文档只保留**当前状态**与**已完成/未做速览**。过程性记录已移除——
> 属历史审计轨，不进活文档；如需追溯见 `git log` 与旧版 `PROGRESS.md` 的 git 历史。
>
> 代码注释里写的 `PROGRESS.md §3l`、`§(5)` 等旧章节号指向的是**那里**——本文档精简后
> 已没有这些小节（约 40 处注释仍用旧写法，按此转发阅读即可）。

---

## 当前状态

**M0–M11 ✅ · Phase 0/1/2/4 ✅（多数）· Phase 3 解码侧 ✅、显示侧 GPU 上屏 ✅（非零拷贝）、真零拷贝 ⬜ · Phase 5 转码代码全落地 + 编码链路实测（硬件加速本体待其他机器）+ `xmedia-inspect transcode` 生产入口 ✅ · macOS 窗口播放器 ✅ · Qt 播放器骨架 ✅（时间线改为真实 `MediaInfo`）· Qt 门面已库化 ✅ · Qt 目标已进 CI ✅ · CI 风格门禁全绿 · 管线原生多源 ✅ · M15 C ABI ✅ · M13 SDK 文档七份 + apidocs 门禁 + 微基准 10 项 + e2e/渲染 CPU 机器判据 + 新鲜度门禁（比库也比源码）✅（剩 golden 基线录制、conan/vcpkg 实跑、soak 48h，均环境阻塞）**

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
- **M15 C ABI**：`player/public/c/xmedia_c.h` 真通用门面（60 导出函数 + 门禁）—— ✅
- **管线原生多源（docs/14）**：`CompositeDemuxer` 聚合 N 源 / 每源一对渲染腿 / 时钟多轴 —— ✅
- **CI 风格门禁**：`check_invariants` / `clang-format` / `cpplint` / `check_binary_freshness` 全绿；`macos-player` 已修 `buildPresets.targets` 截断陷阱（只编 `play_sdl2` 漏掉 `player_unittests`，CI 长期隐形红）。
- **选项面**：构建注册表内 **10 个 key 全部有下游读取者（死选项 0）**——4 个 buffer key 与 `nodisp`/`enable-accurate-seek` 都是先写读取者再注册回来的；`volume`（2026-10-26 补）把 ijk 的 0..100 换算进 `audio.startup_volume`，而该字段现在真的被 `PlayerImpl` 的构造函数读取（此前只有校验、无人读，见 §3）；仍未接线的只是 DRAFT `.inc` 里的 spec key，见 [TODO](TODO.md) §2。

---

## 仍在进行 / 未做

| 项 | 状态 | 说明 |
|---|---|---|
| Ubuntu 真机构建修复（2026-11-16，GCC 11 / FFmpeg 7.1.1 pinned） | 🔶 | **构建已通、测试 96%**：①`composite_demuxer.h` 菱形继承 `Host` 二义性（GCC 11 严格拒判，`using Host = Demuxer::Host` 消歧）；②GCC 11 传递包含差（bind.h `<functional>`、gl_transition.cc `<cmath>`、video_color_space.cc `<climits>`、audio/video_filter_stage.h `<functional>`、三个测试 `<cstring>`）；③GCC 11 对 std::optional 的 maybe-uninitialized 误报（hls_m3u8_parser.cc 定向 pragma，GCC 12 修复）；④`tools/setup_ffmpeg.sh` 新增 zimg 依赖（`--enable-libzimg` 一直硬依赖但不在 deps 机制里）+ 静态 zimg 需要 `--extra-libs=-lstdc++/-lm`（add_extralibs 累加语义）；⑤`av_includes.h` 的 `xmedia_stream_side_data` 补 FFmpeg 4.x 签名兼容（int*/非 const）。**43/1024 失败分三组**：(a) 31 个 GL 像素测试在本机 NVIDIA GLX 上画面方向/时序不对（ScenePresent/ElementComposer/MultiSource/Rotation/Orientation/PlaneUpload/UploadFallback——路径不经过本次改动，macOS CI 绿、本机既有环境问题，待 NVIDIA X11 环境 triage）；(b) `PlayerDecoderEventTest.NormalPrepareReportsNoFallback`——**已修复（2026-11-16）**：根因是 spec 的 codec 列表与 pinned FFmpeg 构建的 hwaccel 清单脱节（nvdec-only 构建里 `av_hwdevice_iterate_types` 连 VAAPI 类型都不枚举，设备探测天然放行）。修法三层门进 factory：`FFmpegHwDecoderSpec` 新增 `verify_build_hwaccel`（Linux 两个 spec 设 true——build 门查 `avcodec_get_hw_config` 是否真的能 (codec, device_type) 放置，NONE 枚举=构建未编=必然失败=decline）+ `device_available` 回调（探测缓存 per-process，无回调的 spec 行为不变，VideoToolbox 工厂套件保持原语义）；decline 走 "not me" 路径不计 fallback。判据：PlayerDecoderEventTest 3/3、FFmpegHwDecoderFactoryTest 2/2（含突变：日志可见 "'VaapiDecoder' declined: this FFmpeg build has no vaapi hwaccel"）；(c) `PlayerTranscodeTest` copy 转码 + `RtmpPublishTest` + `VideoConverterBgraTest`——pinned FFmpeg 7.1.1 构建下 muxer 报 "Video codec unknown"（copy 流 codecpar 未带 codec_id）、BGRA→YUV 色度饱和，转码链不经过零拷贝改动，待 FFmpeg-7.1.1-on-Linux 专项 triage |
| Phase 3 显示侧 GPU 上屏 | ✅ | `apps/player_qt` 把 YUV→RGB 交给着色器并画进 Qt 的 FBO（`QQuickFramebufferObject` + `platform/qt/`）|
| Phase 3 真零拷贝（Linux 显示侧） | 🔶 | **代码全落地（2026-11-16，Ubuntu 真机）**：①解码侧 `media/ffmpeg/vaapi_dmabuf_adapter`——`vaDeriveImage + vaAcquireBufferHandle(DRM_PRIME)` 逐帧派生 per-plane prime fd（NV12/I420，10-bit 诚实拒绝），经 `VideoFrame::set_dmabuf_info`（`DmaBufFrameInfo` 纯数据，media/base 不见 VA 类型）随帧发布，cleanup 并入帧的 release_cb 且先于 AVFrame 释放；②显示侧 `platform/hwimport/egl_dmabuf_importer`——`eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT)` 按 plane 建 R8/GR88 EGLImage，`glEGLImageTargetTexture2DOES` 绑定（绑前清空 GL 错误队列、绑后查错，绝不为拷贝报 kHwZeroCopy），GLX 主机无当前 EGL display → 诚实降级；③工厂 `CreateHwTextureImporter` Linux 分支接上；④`XMEDIA_ENABLE_VAAPI` 从死选项接进 xmedia_ffmpeg/xmedia_player/xmedia_transcode（libva find + 编译定义），`prefer_dmabuf_zero_copy` 获得读取者；⑤新增 `cmake/FindEGL.cmake`（CMake 3.22 无自带，EGL 后端此前永远静默跳过）；⑥`linux-native` 预设修活 + 新 `linux-zero-copy` 预设；⑦判据 `tests/unit/gl/egl_dmabuf_import_unittest.cc`（macOS 编译为空 TU）：CanImport 矩阵 / 无 display 拒绝 / 无 plumbing 拒绝 / **拒绝→kCpuSync 回退契约（本机 NVIDIA 实测过）** + 像素级零拷贝三连（dmabuf→EGL→GL 采样回读，NVIDIA nvidia-drm dumb buffer 生产真 dmabuf）。**本机实测边界**：NVIDIA RTX 5000 Ada 私有驱动 GL（core 3.3 / compat 4.6 / ES 3.2 全测过）对**非自有分配器**的 dmabuf 拒绝绑定（EGLImage 0x502、memory-object 0x505）——像素级断言按绑定能力探针门控跳过并记录原因，在 Mesa（Intel/AMD VAAPI 真机）上生效；**待真机验收**：Intel/AMD VAAPI 机器跑 `linux-zero-copy` 预设看 kHwZeroCopy 像素三连 |
| Phase 3 真零拷贝（NVIDIA 显示侧） | ✅ | **NVDEC→CUDA-GL interop 全链路落地并本机实测通过（2026-11-16）**：①解码侧 `NvdecHwSpec`（cuda hwaccel，h264/hevc/vp9，profile 字符串门拒绝 10-bit/4:4:4——High 4:4:4 Predictive H.264 的 FFmpeg 实测 refusal 在 avcodec_open2 后首包，正是 config_supported 预防的场景）；`FFmpegHwVideoDecoder` 发布 kNvdecFrame + `NvdecFrameInfo`（双 CUdeviceptr 平面 + stride + CUcontext，纯数据过 C5）；②显示侧 `platform/hwimport/cuda_gl_importer`：`glTexStorage2D`(R8/RG8)→`cuGraphicsGLRegisterImage`→`cuMemcpy2D`(GPU 内)→采样；③Linux 工厂改 **chain**（egl-dmabuf + cuda-gl 按 CanImport 路由）；④`XMEDIA_ENABLE_NVDEC` option + CMake 接线（CUDA 头 + **直接链接 libcuda——dlopen 会产生第二个驱动实例，所有句柄 201，实测教训**）；⑤`setup_ffmpeg.sh` 新增 ffnvcodec 依赖 + Linux hwaccel flags（`--enable-cuda/nvdec/hwaccel=h264_nvdec,hevc_nvdec,vp9_nvdec`）；⑥判据 `tests/unit/gl_cuda/nvdec_interop_unittest.cc`：真 NVDEC 解码 testdata H.264 → 生产 chain → **kHwZeroCopy** + luma/chroma 逐字节对比显式读回（76800 字节全对）。本机 3/3 通过。实测踩坑：register 必须在 ctx push 之后（无 ctx → INVALID_CONTEXT）；纹理必须先有 immutable storage；FFmpeg ctx 句柄可能 201 → primary ctx 回退（同 GPU 跨 ctx 设备指针内容正确，实测）|
| Phase 3 真零拷贝（Windows 显示侧） | ⬜ | WGL_NV_DX_interop 未实现；Linux 侧（VAAPI-EGL + NVDEC-CUDA-GL）已闭环 |
| Phase 5 E5 硬件编码器实测 | 🔶 | 编码链路已实测（VideoToolbox：30 帧 → 30 包 → 解回 30 帧，`hardware_encoder_unittest.cc`，突变验过）；**硬件会话本体未实测**——本机 Intel Mac 建不起来（-12903，仅 `allow_sw=1` 的软件实现可用），需 Apple Silicon / Linux VAAPI 机器。为让硬件编码器可用，`FFmpegVideoEncoder::Params` 与 `VideoEncoderFactory::CreateEncoder` 新增私有选项通道（VT 的 `allow_sw`、VAAPI 的 device 都靠它），并经 `TranscodeParams::VideoCodecSpec::options` 贯通到转码路径——CLI 侧是 `xmedia-inspect transcode --encopt allow_sw=1 --vcodec h264_videotoolbox`（实测产出真 H.264） |
| 推流 / RTMP publish | ✅ | `xmedia-inspect publish <file> -o rtmp://… --format flv`（2026-10-28）。`TranscodeParams::output_format` 让无扩展名的 URL 也能定容器（缺了就失败并说明原因，不静默写空流）；`TranscodeParams::realtime` 按输入时间戳配速（3.029 s 源：实时 3.02 s / `--no-realtime` 0.05 s）。真协议验收 `tools/check_rtmp_publish.py` 用 FFmpeg 的 `rtmp_listen` 当服务端（不需要 MediaMTX），回环收到 2 流、时长一致，并断言两种配速模式确实不同 |
| 缩略图 / seek preview 批量取帧 | ✅ | `PreviewFrames()` + `xmedia-inspect preview -o <dir> --start S --count N [--interval S]`（2026-10-28）。关键帧 seek 会落在目标之前，落盘前按时间戳丢弃早于目标的帧（`--interval 0.5` 得到 0/500/1000 ms，而不是 0/0/1000）|
| 录制（播放侧落盘） | ✅ | `PlayerRecorder`（v2，tap 挂点版）：tap 在管线解码流消费侧，copy 模式直写 EncodeMuxer，muxer 写收敛到专用录制线程（双腿并发写 av_interleaved_write_frame 实测死锁）。判据 `player_ffmpeg_unittests` 3 条，共享树下 30/30 压测。三个竞态坑已修（2026-10-31）：muxer 拒绝 post-header 加流 + 录制器挂起队列等双腿流头齐才写 header（否则输家流的 `priv_pts` 未初始化，首包 `frac_add` 除零）；`Start` 向当前播放位置重锚（音频算法队列可预取 3 s，中段挂载会让一条腿整条饿死、容器里留死轨）；首条 EOS 挂 500 ms 兜底收尾。**render-tap 完整形态**（音量/暂停语义进录制）仍为后续项 |
| Qt 门面 | ✅ | 可复用库 `player/qt/`（`xmedia::qtfacade`）已抽出，`apps/player_qt/` 只剩 `main.cc` + `qml/` 并链接它；时间线为真实 `MediaInfo`，`qt_unittests`（`tests/unit/qt/` 8 条）改为链接库、突变验过；M15 C ABI 已作 UE/跨语言挂点 |
| Qt 目标进 CI | ✅ | 新增 `qt` preset + CI job（Ubuntu 24.04 的 Qt 6.4.2）+ `gate_local --jobs qt`：建 `xmedia_player_qt` 与 `qt_unittests` 并跑 ctest（本机 625/625，含 8 条 `TimelineModelTest`）。判据经突变验证：摘掉 `gtest_discover_tests` 后门禁变红（不是静默绿）|
| soak 48h 长稳 | ⬜ | 脚本 `tools/run_soak.py` 已就绪，48h 未跑（沙箱禁读他进程 RSS） |
| Windows CI 验证 | ⬜ | job 已加（no-ffmpeg，不开 `/WX`），首跑待回答 |
| M13 发布 0.1.0 | 🔵 | 文档七份 + apidocs 门禁 ✅；V9 注释覆盖率 / V10 API 兼容 / 微基准 **8 项**（新增 `BM_DecoderBufferQueue_PushPop` / `BM_DecoderBuffer_MoveConstruct` / `BM_VideoFrame_RefCountChurn`，已进基线） / 端到端 CPU 判据 / golden diff 件 / cpack TGZ / vcpkg port 均已完成并入门禁。剩：golden **录制**基线（需 legacy 原版）、依赖解码器的 4 项 `BM_*`（需开 FFmpeg 的 bench 变体）、`conan` 实跑（本机 pip 装不上，网络不可达）、`cpack -G DEB` 待 Linux 机、vcpkg 未提交 registry。**渲染 CPU 的机器判据不做**：`tools/bench_render.sh` 要 ffmpeg CLI 与真实 display，本机两者都没有 |
| M14–M18 | ⏳ | VAAPI+HDR / Android / iOS / 边下边播缓存（后置增量） |
| 产品化能力（Android/iOS/Web/Unity/UE/Flutter/…） | ❌ | 全部未做，见 [TODO](TODO.md) |
| HDR 检测 + tone mapping | ✅ | 检测按传输特性（PQ/HLG），`kHdrUnsupported` 在 `kPrepared` 前上报；**tone mapping 已真做**：`zscale=t=linear:npl= → tonemap=hable:desat=0.5 → zscale=t=bt709`，接在 `VideoConverter` 里并排在 libyuv/sws 之前。实测同一帧 naive 122 / tone mapped 235。三层判据，关掉转换时单元测试报 148vs148、e2e 报 avg luma 123.4 |
| `kRotationChanged` | ✅ | 与 HDR 同类（值在 `video_rotation_` 里、事件零生产者）。现在源声明旋转时才发；测的是「事件值与 `video_rotation()` 必须一致」。角度是顺时针（270 = 90 逆时针 matrix），已核对 `gl_layer_geometry.cc` 的 CLOCKWISE 表 |
| 读源录制 | ✅ | `xmedia-inspect record`，copy 优先，`--duration` 有界；判据量输出时长（40 s 源配 `--duration 3` 必须约 3 s）。**播放侧取包录制未做** |
| `kDecoderOpened` / `kDecoderFallback` | ✅ | 通道 `DecoderStream → 子 renderer → RendererImpl → PipelineImpl → PlayerImpl → EventHub` 接通，另有四个查询。踩坑：WeakPtr 跨序列 CHECK 中止（改 Unretained）、参数顺序错位会把「打开」报成「回落」 |
| 转码链的像素正确性 | ✅ | 10-bit / NV12 源此前转码「成功」但画面损坏（muxer OK、时长对、退出码 0，SSIM 0.035）；`AvFrameToVideoFrame` 改为按源格式分派（I420 走逐平面快路径，其余走 `ff::VideoConverter`），修后 SSIM 0.883。两条测试钉像素值，突变验过 |
| 决策项 | — | LL-HLS 自研 vs 接受 ~3s、M12 原生后端是否启动、版权主体、golden 基线锁版本（开放决策项）|

> 完整开放项、降级项、死选项、环境项见 [TODO](TODO.md)；
> 已修关键缺陷（关闭竞态 / VOD 死锁 / 解码器丢包 / corpus 三崩溃 / EOF 后 seek 永不回调 /
> 暂停中 seek 画面跑飞 / 丢帧计数变成刷新率函数 / fps-cap 判错对象 · 丢帧原因说不出来 /
> 起播后画面定格 / 时间线面板假数据）记录在本表下方状态栏。

# CicadaPlayer 能力移植进度（PROGRESS）

> 活文档。状态标记：`[ ]` 未开始 · `[~]` 进行中（必须带 `since:` 时间戳）· `[x]` 完成（带证据）。
> 原计划书 PLAN.md（范围裁决 §0 / DoD §1 / 核销表 §1.5）已随文档精简移除，内容可经
> `git log --all -- docs/cicada-port/PLAN.md` 找回；本表自足记录剩余与完成。

**当前整体状态**：T1–T9 完成（2026-10-29）+ T8 三条延伸收口（UTC/追帧/DASH 静态，2026-10-31）
+ DASH 动态侧（live 重载窗口，2026-10-31）。CicadaPlayer 移植范围内**无未开题项**；
DASH 的 SegmentList/SegmentBase、多 Period、代表切换仍按 §"DASH 后续"登记。

## 剩余移植工作（PLAN §0 裁决为"部分"或"后置"的部分）

- [x] **T8a UTC 同步**（2026-10-31）：`hls::ParseIsoDateTimeMs`（自研 days-from-civil，Z/±HH[:]MM/闰日）+ `Segment::program_date_time_ms` 解析 EXT-X-PROGRAM-DATE-TIME；demuxer 经 `Host::OnUtcAnchor` 发布锚点（CompositeDemuxer 补转发）→ `Pipeline::Client` → `kUtcTime` 事件缓存 + `Player::GetUtcTimeMs()` 外推查询。判据 4 单测（解析 3 形状/闰日/拒绝；锚点事件 wire + 无锚拒绝）。多源时最后写入者赢（与主时间线=source 0 一致）。
- [x] **DASH demuxer 集成（静态 SegmentTemplate）**（2026-10-31）：`media/ffmpeg/dash_segment_demuxer.{h,cc,_open,_read}`（C1 三拆）。按 mime/带宽选 Representation（TS 复用 rep 一条链覆盖多类型；fMP4 init 段取一次、逐段前拼）；逐段 FFmpegDemuxer + 统一 SegmentTemplate 网格 rebase；跨段读无缝、seek 落目标段（多链 seek 栅栏）；（动态 MPD 当时结构化拒绝，已由下一条的 live 重载窗口接上）。`.mpd` URI 进 factory。判据 4 e2e（打开暴露流/跨段单时间线/seek/动态拒绝）。
- [x] **DASH 动态侧：live 重载窗口**（2026-10-31）：`mpd_parser` 解析 `availabilityStartTime`/`publishTime`/`minimumUpdatePeriod`/`timeShiftBufferDepth`，动态 manifest 由 `(publishTime − availabilityStartTime)` 落在段网格上定位窗口（不再拒绝）；`dash_segment_demuxer_live.{h,cc}` 按最小更新周期（钳在 0.5–30 s）重取 MPD，**按 URL 尾部对齐就地延长**各链而不重建（重建会让游标下的索引重编号、时间线跳变），并按 rep id 查找以免重载时静默换档。直播链尾**挂起读而不发 EOS**（发 EOS 会让最后一次 manifest 的地平线成为流终点），重载补到新段后重发。判据：2 条解析单测（窗口展开 11 段/无锚点结构化拒绝）+ 1 条 e2e（本地 HTTP 服务发布 2 段→重发 4 段，新段可播）。
- [ ] **DASH 后续**（登记不开题）：多 Period、SegmentList/SegmentBase、代表切换（对齐 HLS 的 ABR 半边）、UTCTiming 时钟同步。
- [x] **T8b 直播追帧**：`media/filters/live_edge_policy.*` 已落地（config.net.live_max_latency 驱动），2026-10-31 核对补记。
- 不移植（范围裁决，勿重开题）：Android/iOS/Flutter 平台栈、MediaCodec 原生集成、HDR10(AVFoundation)、插件机制/DCA。

## HyperFrames 对齐移植（2026-11）

- [x] **元素元数据层**（79ac162）：`media/base/html_element_meta.*` + `html_rects_payload`（自研 JSON 子集解析），`mc.html.rects` 协议，15 单测。
- [x] **GIF/ProRes/VP9/HDR 输出**（2528d14/81cc486/b99061d 之前的 8b029c1）：中位切分调色板（AVPalette32 字节序单测钉死）、prores_ks yuv422p10le、libvpx/WebM、hevc_videotoolbox P010LE BT.2020/PQ。
- [x] **CEF OSR 集成**（8b029c1）：cef_runtime/osr_client/app/layer_source/platform_{mac,linux,common}；`mc.html.ready/error/seekDone/rects` MessageRouter。
- [x] **音频滤镜预设层**（此前会话已落，2026-11-07 补登记）：`player/audio_filter_presets.{h,cc}`——合成 SDK AudioFilterType_e 五类预设（降音量/均衡器/回声/降噪/变声）翻成 FFmpeg graph 字符串（aecho/equalizer/anlmdn/asetrate+aresample+atempo，替代 MC 的 sox 后端），参数越界钳制；`tests/unit/player/audio_filter_presets_unittest.cc` 逐预设经 FFmpegAudioFilter 实跑（graph 解析即契约）。
- [x] **T9a 静态帧去重**（2026-11-07）：根因是 HashImageRows 的 stride/width 契约误用（width_bytes 传了 plane 总大小）；改用 HashBytes64 对 `visible_data(kYPlane)` 整段扁平 hash——无 stride 推算、无越界面。`dedup_skipped_frames()` 诊断计数器 + 单测（同内容连续帧 dedup=4 且 pts 网格不破、内容变化即停）。
- [x] **T9b 验证 seek**（2026-11-07）：`media/transcode/encode_verify.{h,cc}`——转码后重开输出、按采样 pts seek 回读解码、32×18 luma grid 比对；`TranscodeParams::verify_output_frames`（默认关，sidecar `<out>.verify.json` 双向登记，失败 kVerifyFailed 带帧号）。2 单测：整管线 round-trip 通过 + 伪造签名被检出。
- [x] **CEF OnPaint 冒烟 .app 实跑通过**（2026-11-07，撤销此前"结构性卡点"登记）：此前登记的 GPU/network 子进程 dyld 限制并非根因。真实根因三件：① POST_BUILD `make_directory` 把二进制路径建成目录（copy 目标成为文件夹）；② 测试页缺失；③ `cef_load_library` 非幂等（`g_libcef_handle` 已设时返回 0），宿主预装载后 Init 再装载被误判失败——`CefPlatformLoadFramework` 加 `RTLD_NOLOAD` 常驻探测。现在 `media_cef_smoke.app`（framework + Helper + bundle 布局）端到端 PASS：OnPaint 帧发布 + 元数据采集。元素拆纹理 GPU 渲染的前置条件已满足（quad/UV/映射纯逻辑在 79ac162 就绪）。
- [x] **元素拆纹理 GPU 渲染**（2026-11-07）：前置打通 + 模块落地。① `GlVideoLayer` 加 packed-RGBA 上传路径（kARGB 单纹理 GL_BGRA 直传 + RGB passthrough shader 双变体 + `is_rgb()`），此前只收 planar YUV、CEF 的 kARGB 帧会被 Upload 拒绝——这是此前"元素拆纹理"真正的隐性前置；② `GlScene` 构建时一并创建 RGB 程序对（构建期暴露编译失败），draw 按 `is_rgb()` 选程序；③ `media/gl/gl_element_composer.{h,cc}`：BuildElementQuads (z, ordinal) 稳定排序 → 每生效元素一个场景层（同帧上传、SetLayerSourceRect 采 UV 窗、SetLayerRect 定位、SetLayerZ 用 data-z，与其它场景层同 z 序交错）；消失元素隐藏层不复建；帧指针不变不重传。与 CEF 解耦（输入 = 任意 VideoFrame + 元数据表）；`CefLayerSource::last_frame()` 供宿主取整层图。像素验收 4 条：双元素各落其位（未被元素覆盖的象限必须是背景——防"整层糊上去"退化）、生效窗外缺席、消失隐藏、z 管线成立（z 序像素不可判如实登记：同一内容映射下重叠 quad 必采同 texel）。GL 套件 74/74。登记后续：共享纹理 + 多 quad 绘制优化（当前 N 元素 = N 份纹理拷贝）。
- [x] **VP9 alpha（2026-11-07）**：`FFmpegVideoEncoder` alpha 分支——`options["alpha_mode"]="1"` + libvpx-vp9 且编码器声明 yuva420p 才启用（I420 输入没有 alpha 面，sws 到 YUVA420P 的 A 面是未定义字节）。`in_fmt=AV_PIX_FMT_BGRA`（kARGB 字节序 B,G,R,A；`ToAvPixelFormat(kARGB)` 的 0RGB32 在 LE 下把第 4 字节当忽略位，不能用于 alpha）→ sws → YUVA420P → avcodec_open2 时 `pix_fmt==YUVA420P` 自动激活 FFmpeg 7.x 的 `is_alpha`（不需要 alpha_mode 私有选项——7.1.1 的 libvpxenc 已改成按 pix_fmt 激活）。**alpha 不在码流字节里**：FFmpeg 7.x 放在 `AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL` side data 上（DrainPackets 已透传），解码只有 libvpx-vp9 解码器读它（原生 vp9.c 无 alpha 路径）。回读单测：不透明红 A=255 / 全透明绿 A=0 → 解回 YUVA420P 且 A 面按帧保真。顺带修一个堆损坏：测试用 `av_packet_from_data` 会把 vector 的 new[] 内存挂上 `av_buffer_default_free`（av_free 释放 new[] = UB），表现为后续测试随机 SIGABRT——改 av_new_packet+memcpy，三连跑全绿。
- [x] **分块编码 + concat（2026-11-07）**：`media/transcode/ffmpeg_chunked_job.{h,cc}`——`ChunkedTranscode`：keyframe 网格探查（目标块时长吸附到 ≤ 目标的最近 keyframe）→ 逐窗 `Transcode`（编码器每块关键帧冷启，块间零依赖，可并行/可续跑的基础）→ `Concat` 拷贝路径无损拼接 → 块文件清理（失败/取消也清理）。前置：`Transcode` 补**帧级 start 裁剪**（视频帧/音频缓冲在窗口起点前的解码后丢弃——seek 只能落 keyframe ≤ start，不裁则块接缝重叠；帧级丢弃不破 GOP；音频整帧丢弃 → 接缝至多 ~21ms 空隙，keyframe 网格下的最优解）。与 HyperFrames bake 分块的差别如实登记：那边每块全新渲染可任意边界，这边对已有输入转码，精确边界只有 keyframe。2 单测：多块拼接时长保真 + start 裁剪帧精度。
- [x] **html_bake 离线烘焙管线（2026-11-11，412ca1a）**：`examples/html_bake.cc`——`GlElementComposer` 的生产消费方落地。CEF OSR（VirtualTime bake 时钟，文档时刻 = 帧序号 × 帧间隔，确定性）→ 每帧 `ApplyTimeline`（页面按 data-start/duration 自切可见性，渲染进程 ACK 回执 `mc.html.applied` 后才步进）→ 元素拆纹理合成 → GlScene → 读回（行序翻转）→ libyuv BGRA→I420 → 编码封装 → `--chunks N` 分块（每块独立编码器冷启 + VT 从 0 快进到窗起点——合成确定性使**任意边界**精确，无 keyframe 约束）→ Concat 拼接 → 块清理。配套基建：`CefLayerSource` caller-thread pump 模式（GUI 宿主的 CEF macOS 主线程约束——pump 线程 Init 与主线程 run loop 互等死锁实测）、`cmake/XmediaCefApp.cmake` 共享 .app 组装、OnLoadEnd 重注入采集/时序脚本、VT 步进 Invalidate（静态页无损伤时 external begin frame 不出帧）。**修出一个真 sws 缺陷**：本构建 FFmpeg 的 sws RGB→YUV 色度计算损坏（luma 全对、色度恒 255/99，4 格式 × 2 尺寸矩阵实测）——VideoConverter 加 libyuv `ARGBToI420Matrix` 快速路径（按帧色彩空间选 ArgbConstants）。ctest `html_bake_smoke` 通过；像素验证位置/颜色/时序全对。已知边界（如实登记）：① 元素出生效窗首帧可能缺元素（CEF 渲染器对 visibility 恢复的重绘惰性，内容验证式重绘可缩小不可消除；HyperFrames 原版页面框架自持渲染故无此问题）；② 平铺光栅裁切对重叠元素透底（非重叠布局是精确前提）。
- [x] **共享纹理优化（2026-11-11）**：`GlVideoLayer::ShareTexturesFrom` 纹理别名视图——Bind/texture_target/ready/width/height/source_aspect/is_rgb 全量代理 source（任何代理漏项都以黑屏呈现，is_rgb 漏代理实测全黑）；视图不拥有 source 纹理（各自 planes_ 独立分配、析构无别名泄漏）；source 每次 Upload 后必须 re-Share（纹理对象可能整体重建，有重建弹性像素测试）。`GlElementComposer` 改 master+views：一个隐藏 master 层唯一 Upload，N 元素层为视图——**N 元素 1 份纹理拷贝**。`Config.share_textures` 可关（回退路径有像素一致性测试）。GL 76/76（含 2 条新测试）。
- [x] **Seek bake 时钟 + 数据注入 + 逐属性关键帧 + 元数据导出接线（2026-11-11）**：对照 MC 源码审计后补齐的四项差距。① **Seek 时钟**（MC `html_bake_clock` 的**默认值**）：`window.seek(t_ms)` 页面契约（页面自发 `mc.html.seekDone` 回执，bake 放行；页面无 seek 时 `mc.html.error:no_seek` → 回退预应用时序）——页面自持时间线时**帧级精确**（seek_smoke.html 实测 chip 恰在 t=500 帧出现），`--clock seek` 接入 html_bake；② **页面数据注入**：`SetHtmlData`（ValidateHtmlDataJson 安全门——无 jsoncpp 的最小 JSON 状态机，拒绝非对象/数组根防脚本注入）→ 首次出帧后经 `window.update(d)` 契约注入（data_smoke.html 实测 databox 按 data 变色）；③ **逐属性关键帧**：`media/gl/layer_keyframes.{h,cc}`——EaseType（CSS 具名 + 自定义 cubic-bezier，WebKit UnitBezier 数学：Newton-Raphson + 二分兜底）+ `EvaluatePropertyKeyframes`（区间外钳制/区间内按区段首 ease 插值）+ GlElementComposer 整层动画（opacity→元素层、x/y/scale→内容矩形；rotation 与元素拆纹理语义不等价，如实登记不移植）；7 单测（端点钳制/对称中点/缓动方向/整层求值）+ 不透明度淡入像素测试。④ **元数据导出接线**：html_bake 产出 `<out>.html_elements.json`（BuildElementsMetaJson 的真实调用方，MC render_demo 对齐）。**顺带修掉一个真缺陷**：AwaitTimelineApplied 在 caller-thread 模式用 CV wait_until 阻塞——回执需要同线程 Pump 处理，自阻塞成每次 2 秒死等（改自旋后 4 ack miss → 0，烘焙全程 4s→快速）。
- [x] **TickBakeClock + Lottie 元素（2026-11-11，审计 #5/#6）**：① **TickBakeClock 统一时钟分发**（MC 对齐）：VT = credit/帧；seek = 同 t 去重 + seek 在飞门控的 window.seek；seek 不支持 = live no-op——html_bake 的时钟分支收敛进 facade（补齐一桩旧账：Impl 的 OnHtmlSeekDone/no_seek 处理器在早前脚本断言中断时从未落盘，seek 验证靠 2 秒超时兜底暗耗每帧 2 秒，补上后真 ACK 生效）。② **Lottie 元素**：`tools/setup_rlottie.sh`（rlottie 0.2 静态库最小化构建，MC 离线源码可重建）+ CMake `XMEDIA_ENABLE_LOTTIE` 门控（缺库降级 OFF，对齐 CEF 模式）+ `media/lottie/lottie_clock.{h,cc}`（帧号映射：floor(t/1000*fps*speed)、loop 回绕/非 loop 钳制、表面尺寸钳制；8 单测）+ `media/lottie/lottie_animation.{h,cc}`（rlottie::Animation 封装：renderSync 纯函数式 CPU 光栅化 → premultiplied BGRA → un-premultiply → kARGB 直通 RGB 上传路径）+ html_bake `--lottie <path>`（叠加层 z 垫在 HTML 元素下，bake 时钟驱动帧号）。端到端像素验证：fade_box.json 淡入 10%→50%→90% 逐帧精确。
- [x] **HyperFrames 原版契约对齐（2026-11-12，对照 heygen-com/hyperframes npm v0.9.33）**：审计发现 MC 支线的 `window.seek`/`mc.html.*` 是自造协议，原版真实契约为「页面注册 paused GSAP 时间线进 `window.__timelines` + 驱动方遍历注册表 `pause + totalTime(秒+1e-3)`」。落地：① 原版 runtime vendor（`assets/hyperframes/runtime.iife.js` 653KB + `media/cef/hyperframes_runtime.inc` 内嵌）；② **页面脚本前注入**：`MediaCefApp::OnContextCreated` 主 frame 注入（V8 上下文创建时页面脚本未运行，evaluateOnNewDocument 等价）；③ **驱动**：MakeSeekScript 优先 `__hf.seek(秒)`（官方管线薄包装，前向兼容）→ 注册表迭代 `pause + totalTime`（权威契约，motionShot 模块对齐）→ window.seek（MC 契约）→ no_seek；完成回执经 `__hfWaitForSeekCompletion` 桥接为 seekDone；④ **采集器词汇扩展至原版全集**：data-end/track-index(+layer legacy)/media-start/playback-start/playback-rate/lottie-src/composition-id（采集 + 解析 + 粘滞 merge + 字段）。验证：hf_gsap_smoke.html（原版模板结构：composition-id 根 + clip + `__timelines["main"]` 注册 + 内联 GSAP）——**box 按 GSAP 时间线逐帧位移（0→600ms 位置精确）+ 两次烘焙 11MB 像素逐字节一致（确定性 ✓）**。新增认知：file:// 页面的 file:// 子资源（script src）在 CEF OSR 整页黑（已有 `allow-file-access-from-files` 开关实测无效），离线测试页用内联脚本绕开；HF data-duration 单位是**秒**（blank 模板 `data-duration="10"` = 10s 实证），与 MC 毫秒制不同。已知边界：页内 runtime 会接管合成（seek 后 body 背景转黑、加速 canvas 自绘）——真实 HF 页面按 skill 契约自带脚手架，最小测试页缺脚手架的背景差异如实登记。七套件 874 + ctest ×2 全绿。
- [x] **MC 回移完成（2026-11-12，MC 74d43b32d）**：同款契约对齐落地 MC（framework/render/）：runtime .inc 内嵌 + OnContextCreated 注入、MakeSeekScript 驱动链（__hf.seek → 注册表迭代 → window.seek → no_seek）、采集器词汇全集（collector + jsoncpp 解析 + 粘滞 merge）、单测 +2（词汇解析 + 粘滞语义）89/89 全过。E2E bake 验证受限于本 shell 环境 MC html-bake 启动竞态（IOSurface err=10007、pristine stash 实测同样失败——环境问题非回移问题，MCP html_bake_submit 管线可作替代验证路径）；契约等价性由 xmedia-kit 887ec02 同源移植的端到端确定性验证背书。
- [x] **原版模板单位适配 + .clip 生命周期边界实证（2026-11-13，707d29c）**：`html_bake --hf-seconds`（HF data-start/duration/end 为秒制，blank 模板 `data-duration="10"` = 10s 实证——采集元数据 ×1000 归一毫秒）。**剩余集成边界（如实登记）**：注入 runtime 会接管 `.clip` 元素显隐（blank 模板 Title 隐藏；无 .clip 页面渲染/驱动正常，数据驱动的自定义 clip 结构亦正常）——runtime 的 clip 揭示协议需官方管线握手，注册表 totalTime 仅驱动 GSAP 状态；完整 `.clip` 生命周期支持 = 后续研究 runtime 揭示协议（或以无 .clip 的数据驱动页为 xmedia 烘焙的目标页型）。

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

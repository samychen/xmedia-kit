# `media/` 入门学习指南

> **面向**：读完了 [`LEARNING-base.md`](LEARNING-base.md)、要继续看懂播放器核心的人。
> **基线**：commit `2deca4a`（工作区干净，故 HEAD == 工作区）。
> **方法**：与 `base/` 那篇相同 —— 双轴审查（明文标准 / 规格清单）并行跑，
> 再**逐条复核**；两条轴的结论分开呈现、不合并排序。
> **状态**：未提交。

---

## 0. 怎么用这份文档

`media/` 是 **214 个受版本控制文件、约 34,600 行** —— 是 `base/` 的 8.5 倍。
不要按目录顺序读，**按一帧数据的生命周期读**：

```
DataSource ─▶ Demuxer ─▶ DemuxerStream ─▶ DecoderStream ─▶ 解码器
                                              │
                          VideoRendererImpl ◀─┤─▶ AudioRendererImpl
                                  │                    │
                          VideoFrameCompositor    AudioRendererAlgorithm
                                  │                    │
                               VideoSink          AudioSink
                                  └──── AvSyncController（主时钟）
```

每一站都按「**文件 → 它解决什么问题 → 初学者该注意什么 → 去哪儿看用法**」组织。
先看接口（`media/base/`），再看实现（`media/filters/`、`media/ffmpeg/`）；
接口决定了你能改什么，实现只是它今天长这样。

| 目录 | 文件 | 行数 | 一句话 |
|---|---|---|---|
| `media/base/` | 76 | 8726 | 接口与值类型（**必须是叶子**） |
| `media/ffmpeg/` | 47 | 7086 | FFmpeg 隔离区（唯一能看见 `libav*` 的地方） |
| `media/filters/` | 48 | 11291 | 具体实现（含 `legacy/` LGPL 区） |
| `media/gl/` | 19 | 3743 | 被 `VideoFrame` 塑形的 GL 层 |
| `media/transcode/` | 18 | 3514 | 离线转码产品线（独立 target） |
| `media/renderers/` | 2 | 275 | 渲染器工厂 |

---

## 1. 第一件事：把两条硬边界背下来

`media/` 的一切组织都可以从**两条边界**推出来。它们都由门禁强制，不是约定。

### 边界一 · FFmpeg 隔离区（`media/ffmpeg/`）

规格在 `docs/02` §4.4（第 248-269 行）：**一个目录，一个目标
（`avbase_ffmpeg`），一条判据：需要 `libav*`。** 三条规则守着：

| 规则 | 内容 |
|---|---|
| **C4** | `libav*`/`libsw*` 头只许出现在 `media/ffmpeg/` |
| **C27** | `media/base/`、`media/filters/`、`media/renderers/` 不许 include `media/ffmpeg/` 里的任何文件 |
| **C25** | 每个 `.cc` 都有目标归属 |

**C27 是真正关键的那条**，它的自述很直白（`check_invariants.py:325-331`）：
C4 能抓到一句 `libav*` 的 include，但一个核心文件可以经由
`media/ffmpeg/<helper>.h` 把整个 vendor 层拖进来而**从不写出 `libav*` 四个字**，
C4 看不见这种情形。所以边界必须是**目录**，不是头文件集合。

`media/transcode/` **刻意不在 C27 名单里**（`check_invariants.py:332-337`）：
它建在 `av_includes.h` / `compat.h` / `video_convert.h` 之上，
按设计可以跨这条边界。让 G2 继续成立的是「它是独立 target、
`avbase_media` 从不链接它」，**而不是禁止它越界**。

### 边界二 · `media/` 不碰宿主（C26）

`media/` 不许 include `platform/`；它需要的一切经 `player::Deps` 注入。
这条**直到 FFmpeg 代码从 `platform/ffmpeg` 迁到 `media/ffmpeg` 才真正成立**
——此前 `media/filters` 有 19 个文件直插 `platform/ffmpeg`，规则只是愿望。

### 顺带：`media/base/` 是干净的叶子

实测确认：`media/base/` 对 `media/filters/`、`media/ffmpeg/` **零实际 include**；
平台头与 `__APPLE__`/`__linux__`/`_WIN32` **零命中**（`native_display.cc:18`
的 "wayland-surface" 是字符串字面量，宏检查器已正确放过）。

**它换来什么**：`media/base` 可被任意宿主编解码器或测试替身引用，而不拖入
FFmpeg 符号或平台 ABI；`avbase_media` 的公开头保持零供应商耦合。

---

## 2. 阅读路线

### 第 1 站 · 值类型（先认数据，再认流程）

| 文件 | 看点 |
|---|---|
| `media/base/decoder_buffer.h` | 压缩数据单元：pts/dts/duration/side data/storage |
| `media/base/encoded_packet.h` | 编码侧的同一个形状，跨「编码器 → 复用器」边界 |
| `media/base/video_frame.h` | 解码后视频帧 + `StorageType` + metadata |
| `media/base/audio_buffer.h` | 解码后音频帧 |
| `media/base/audio_bus.h` | planar float 音频总线 |
| `media/base/media_info.h` | 容器/时长/流信息 |
| `media/base/decoder_config.h` | 解码器配置（含 extradata） |

**`video_frame.h:130-136` 的 `NaturalSizeForRotation()` 是好例子** —— 它讲清了
「先适配再旋转」这个**看起来像旋转 bug 的假故障**。陷阱型 API 的注释要描述
**失败长什么样**，而不是描述自己做了什么。

⚠️ 注意 `CreateBlackFrame`（`video_frame.h:186-190`，7 参）与 `WrapNativeBuffer`
（`:198-202`，10 参）—— 见 §4 的 T2。

---

### 第 2 站 · 接口（决定你能改什么）

`demuxer.h` / `demuxer_stream.h` / `video_decoder.h` / `audio_decoder.h`
（都是**异步回调式**）/ `renderer.h` / `renderer_client.h` /
`video_renderer_sink.h` / `audio_renderer_sink.h` / `data_source.h` /
`time_source.h` / `media_log.h`。

**重点对比两个 sink 的析构函数**：

- `video_renderer_sink.h:102-116` —— 析构函数是 **public**，注释明说这是
  load-bearing：`VideoRendererSinkFactory::Create()` 用 `std::unique_ptr` 交回，
  `default_delete<VideoRendererSink>` 要经 `VideoRendererSink*` 调 `delete`，
  需要可访问的析构函数；protected 会让每个 `unique_ptr<VideoRendererSink>`
  在析构点变成 ill-formed。注释还点明这是**本头文件里一直存在的潜在缺陷**，
  只是第十轮之前没人实例化它。
- `AudioRendererSink` 的析构函数是 protected 且**正确** —— 它引用计数，
  `befriend base::RefCountedThreadSafe<AudioRendererSink>`，由 `Release()`
  从内部执行 delete。

**两个 sink 长得像但所有权不同，所以其中一个曾经是错的。** 这是全目录最值得
记住的一条教训。

---

### 第 3 站 · 解复用（`media/ffmpeg/`）

`ffmpeg_demuxer.cc`（928 行，全树最大）、`ffmpeg_demuxer_configs.cc`、
`ffmpeg_demuxer_track_select.cc`、`ffmpeg_demuxer_seek.cc`、
`data_source_io.*`、`url_data_source.*`，以及 `media/base/composite_demuxer.cc`。

**线程模型是有意偏离 Chromium 的**，见 `docs/04` §2.1 与 `docs/02` §9 D3：
这里的解复用器**有自己的 demux 线程**，而不是挂在 media 序列上。

---

### 第 4 站 · 解码

`decoder_stream.{h,cc}`（★模板：解码器选择 + 回退 + 异步泵 + 帧队列）、
`decoder_selector.*`（纯函数排序）、`ffmpeg_video_decoder.*`、
`ffmpeg_audio_decoder.*`、`ffmpeg_hw_video_decoder.*`、
`ffmpeg_hw_decoder_factory.*`、`ffmpeg_decoder_factories.*`。

---

### 第 5 站 · 渲染

`video_renderer_impl.*`、`audio_renderer_impl.*` +
`audio_renderer_impl_render.cc`、`audio_renderer_ring.cc`、
`audio_renderer_algorithm.cc`（★WSOLA 变速不变调）、`wsola_internals.*`、
`renderer_impl*.cc` 六个分片。

---

### 第 6 站 · 时钟与帧调度 ⚠️

**这里有一个初学者一定会踩的坑**：`av_sync_controller.*` 与
★`video_frame_compositor.*` **不在 `media/filters/` 而在
`media/filters/legacy/`**。

`media/filters/legacy/` 是 **LGPL-2.1 隔离区**（有独立 `README.md` 与
`LICENSE.LGPL-2.1`）。判据写在 README 里：

> ✅ A file belongs here **iff** a reviewer could point at a function in
> `ff_ffplay.c` and say "this is that function, transliterated".

**而且阈值是继承的，不是改进的** —— 必须与原值一致，这正是
`tools/extract_constants.py` 校验的内容（`docs/05` 表 7）。在这里"顺手优化"一个
魔法数字是违规的，除非有 golden 测试证据。

注意这是**第二条分区判据**：§4.4 那条是「按依赖」分（FFmpeg 隔离区），
这条是「按算法出处」分（许可证隔离区）。两者正交。

---

### 第 7 站 · GL 呈现（`media/gl/`）

19 个文件。规格在 `docs/02` §6.2（第 381-402 行）：

| | 放什么 | 依赖 |
|---|---|---|
| `render/gl/`（`avbase_gl`，可独立分发） | context 契约、入口点装载器、program/texture/quad、GLSL 方言 | **零依赖** |
| `media/gl/` | YUV→RGB 矩阵、平面拆分与上传、overlay 合成 | `media` + `render/gl` |

判据一句话（第 391 行）：**任何被 `VideoFrame` 塑形的东西留在 `media/gl/`。**
这样 `render/gl/` 才能被一个"从没听过 `VideoFrame`"的项目整个拿走。

方向：`platform/{sdl2,glfw,egl} ──实现 NativeGLContext──▶ render/gl ◀──借用── media/gl`

---

### 第 8 站 · 转码（`media/transcode/`）

18 个文件，独立 target `avbase_transcode`，建在 `avbase_ffmpeg` 之上，
由 `AVBASE_ENABLE_FFMPEG` 门控，**刻意不进 `avbase::avbase` 聚合** ——
否则每个播放构建都要背着编码器与封装写端。

---

## 3. 抄作业区 —— 10 段值得模仿的写法

| # | 位置 | 学什么 |
|---|---|---|
| 1 | `media/base/video_renderer_sink.h:102-116` | 所有权决定的语言细节（public 析构）要连同"为什么"一起写下；并与另一个 sink 对照说明为何一个对一个错 |
| 2 | `media/base/data_source.h:76-84` | 注释**指向规则**（回指 `base/memory/ref_counted.h`），而不是复述结论 |
| 3 | `media/filters/decoder_stream.cc:256-261` | 写明并发修复「防的是什么数据竞争」——初稿直接绑回调被 sequence checker 抓到，且不是形式主义 |
| 4 | `media/filters/decoder_stream.cc:374-383` | 性能/正确性取舍**附实测数据**（89/93 批次剩 7 个 buffer），不附直觉 |
| 5 | `media/filters/video_renderer_impl.cc:149-156` | 写清「**测试为什么没抓到**」比写清"修了什么"更有价值 |
| 6 | `media/filters/video_renderer_impl.cc:166-170` | 显式记录「故意没做的事」（`NOT paused_ = false`）及其后果 |
| 7 | `media/base/video_frame.h:130-136` | 陷阱型 API 的注释要描述**失败长什么样** |
| 8 | `media/ffmpeg/av_includes.h:5-13,96-103` | 供应商边界收成**一个文件**；自带的替代函数用 `avbase_` 前缀标记 |
| 9 | `media/filters/audio_renderer_algorithm.cc:17-23` | 移植的阈值注明「取自 Chromium 哪个标识符、抓取日期」，并声明不是凭记忆抄的 |
| 10 | `media/filters/legacy/video_frame_compositor.cc:34-39,178` | 许可证与血缘是代码的一部分：`ALGORITHM PROVENANCE` 段落在文件头与函数头各写一遍 |

第 9 条背后有具体教训：R1 曾因凭记忆抄 FourCC 抄错。第 8 条的价值在于：
**版本差异集中在一个文件里才可评审**（C8 强制 `#if LIBAV*_VERSION*` 只出现在那里）。

---

## 4. 陷阱区 —— 复核后确认的 8 处

### T1 · 隔离区被绕过（最该先修的一条）

`media/ffmpeg/av_includes.h:5` 自称：

> `THE ONLY PLACE IN AVBASE THAT INCLUDES FFmpeg HEADERS.`

并列了两项职责：① `extern "C"` 包裹；② 派生全部版本开关。

而 `media/ffmpeg/ffmpeg_demuxer_configs.cc:7-9` 是自己手搓的：

```cpp
extern "C" {
#include <libavutil/display.h>
}
```

**这不是格式问题，是把集中化约束废掉了** —— 集中化的价值在于「只有一个入口」，
就地复制包裹代码等于入口形同虚设。**C4 只查「位置是否在 `media/ffmpeg/` 目录内」，
不查「是否走了那个入口」**，所以闸门漏掉它。实测确认：这是 `media/ffmpeg/` 里
**唯一**一个绕过 `av_includes.h` 的文件。

> 复核订正：轴一同时报了「include 顺序违规，本项目块排在 C++ 标准库之前」。
> **这条不成立** —— 实际顺序是 自己的头 → `libav*`（extern "C"）→
> `<cmath>`/`<string>` → 项目头，符合 STYLE §3。真正的问题只有「绕过入口」一条。

### T2 · 两个函数参数超过 6 个

| 函数 | 位置 | 参数数 |
|---|---|---|
| `CreateBlackFrame` | `video_frame.h:186-190` | 7 |
| `WrapNativeBuffer` | `video_frame.h:198-202` | 10 |

STYLE §5：参数 > 6 必须改用 `struct XxxParams`。这两个函数共享
`format / coded_size / natural_size / sar / timestamp / duration` 六个参数 ——
**它们已经在喊着要变成 `VideoFrameParams`**。而 `WrapNativeBuffer` 的后四个
（`serial` / `release_cb` / `to_i420_cb`）本身又是一组「硬件帧生命周期」参数。
参数列表变长是**设计在提示你缺一个类型**，不是提示你换行。

### T3 · `GUARDED_BY` 是注释而不是注解

`media/ffmpeg/url_data_source.h:75-79`：

```cpp
AVIOContext* avio_ = nullptr;  // GUARDED_BY(lock_) for the pointer itself.
bool size_known_ = false;  // GUARDED_BY(lock_)
```

对照 `media/base/decoder_buffer_queue.h:168-172`，那里用的是**真注解**：

```cpp
std::deque<base::scoped_refptr<DecoderBuffer>> buffers_ GUARDED_BY(lock_);
size_t bytes_ GUARDED_BY(lock_){0};
```

STYLE §7 要求成员用 `GUARDED_BY()` 让 `-Wthread-safety` 真正生效 ——
**写进注释等于放弃静态检查**。两者风格不一致，改法是把前者也换成真注解。

### T4 · `decoder_stream.cc` 三处雷同的错误路径

同一形状出现三次，只差 status 来源：

| 行 | status |
|---|---|
| `298-305` | `kDecodeError`（拼上 `"demuxer read: "`） |
| `409-413` | 传入的 `status` |
| `470-476` | `kDecodingAborted`（`"flushed by seek"`） |

另外 `AdoptSerial`（约 `449-455`）与 `Flush`（约 `459-465`）各有一段**五行状态复位**，
内容几乎一致。抽 `DrainPendingReads(status)` 与 `ResetBuffersForNewSerial()` 即可。
**三份雷同的错误路径，就是三处将来只改其中两处的 bug。**

### T5 · `serial` 是裸 `int32_t`

seek 世代号以 `int32_t` 出现在 `video_frame.h:143/188/201`、`decoder_buffer.h:113`、
`demuxer_stream.h:70`、`decoder_stream.cc:315`、`video_renderer_impl.cc:157-160`。
它带比较语义（`serial() != serial_`），是完整的领域概念。给一个 `class Serial`，
比较只能走它的接口；否则 `serial + 1`、`serial > 5` 这类无意义运算编译得通。

### T6 · 消息链

`video_renderer_impl.cc:157` 与 `:232` 都是
`decoder_stream_.demuxer_stream()->serial()` —— VideoRendererImpl 借道
DecoderStream 去问 DemuxerStream。`a.b().c()` 是**耦合声明**：调用方不该知道这条
路径。让 DecoderStream 暴露一个 `current_serial()`。

### T7 · 文档里的旧路径**不是**漂移（一条容易搞反的约定）

`docs/03` 有三处路径对不上真实文件：

| 位置 | 写的 | 实际 |
|---|---|---|
| `docs/03:424` | `#include "media/base/decoder.h"` | `decoder.h` **从未存在** |
| `docs/03:925` | `// media/filters/video_frame_compositor.h` | `media/filters/legacy/video_frame_compositor.h` |
| `docs/03:1189` | `// media/filters/av_sync_controller.h` | `media/filters/legacy/av_sync_controller.h` |

**直觉会把这三条报成"文档漂移、要回填"，那就错了。** README 第 338-343 行有一段
「路径变更通知」，明文写着：

> ① `video_frame_compositor.{h,cc}`、`av_sync_controller.{h,cc}`、`clock.{h,cc}`
> 移入 `media/filters/legacy/`（LGPL-2.1 隔离），**docs/01–08 中的旧路径刻意保留
> 不改** —— 事后改路径会让"当时决定了什么"失真。
> ② `platform/ffmpeg/*` + `media/filters/ffmpeg_*` 整体迁入 `media/ffmpeg/`；
> 这一批是**目录本身已不存在**，故全套文档已回填。

也就是说 925 / 1189 两条是**刻意保留的旧路径**（文件搬走了，但那几篇文档是
历史记录，回填会失真），**不该动**；而 424 的 `decoder.h` 是**规格列了却从没
建过**，性质不同 —— 它归入下面的 T8。

**给初学者的规则**：看到文档路径和真实文件对不上，**先查 README 的「路径变更
通知」**，分清是「搬走了、刻意留旧」还是「压根没建」。这两者的处置完全相反。

### T8 · 三个"没有任何文档交代"的缺失

`video_frame_pool.h/.cc`（规格 L161）、`seekable_buffer.h/.cc`（L184/212）、
`decoder.h`（L172）都不存在，且**没有任何文档说明为什么**。
对比之下另外两类不算 drift：

- `cdm_context.h` / `encryption_pattern.h` —— `docs/01:229` 明确记为非目标
  （「❌ 不做 DRM / CDM，只留空接口位」），只是连空占位文件也没建；
- `media/audio/` —— 规格 §4.3 自己写着「这一目录尚未建立」；
- `file_data_source.cc` —— `docs/05:107` 标 `⏳ M12`。

---

## 5. 规格 vs 实现对照表

规格来源：`docs/02` §4（第 151-300 行）。

### §4.1 `media/base/` 清单逐项

| 规格行 | 文件 | 结论 |
|---|---|---|
| L161 | `video_frame_pool.h/.cc` | **缺失**，无文档记录 |
| L162 | `video_frame_metadata.h` | **改形态**：`timestamp()`/`duration()`/`reference_time()` 内联于 `video_frame.h:140-142`（无 `rtp_timestamp`） |
| L164 | `video_transformation.h` | **改形态**：`rotation()` 内联 `video_frame.h:128/166`，另见 `decoder_config.h:69`、`media_info.h:41` |
| L168 | `channel_layout.h` `sample_format.h` | **改形态**：两枚举内联 `audio_parameters.h:17-38` |
| L170-171 | `video/audio_decoder_config.h`、`video/audio_codecs.h` | **改形态**：统一为 `decoder_config.h`（`VideoCodec` L32 / `AudioCodec` L44） |
| L172 | `decoder.h` | **缺失**（`decoder_status.h` 在，基类不在） |
| L181 | `media_log_event.h` | **改形态**：`struct MediaLogEvent` 在 `media_log.h:35` |
| L184/212 | `seekable_buffer.h/.cc` | **缺失**，无文档记录；`docs/06:806`、`docs/07:52` 仍在清单里列它 |
| L185-186 | `cdm_context.h` `encryption_pattern.h` | **记录在案的非目标**（`docs/01:229`），但连空占位文件也没建 |

**量化**：L188「共约 30 个文件、~4500 行」 vs 实测 **76 文件 / 8726 行**
（文件 2.5×、行数 1.9×）。

### §4.2 `media/filters/` 清单

| 规格行 | 结论 |
|---|---|
| `decoder_selector.h/.cc` | ✅ 在 `media/filters/` |
| `default_decoder_factory.cc` | **化入** `media/ffmpeg/ffmpeg_decoder_factories.*`（符合 L214-226 的搬移说明） |
| `av_sync_controller.*`、★`video_frame_compositor.*` | **在 `media/filters/legacy/`**，规格表没写 |
| `text_renderer.cc` | **化入** `renderer_impl_text.cc` |
| `file_data_source.cc` | 缺失但有据（`docs/05:107` ⏳ M12） |
| `buffered_data_source.cc` | **化入** §4.4 的 `url_data_source.*` + `data_source_io.*` |

### §4.3 `media/audio/`

目录**确实不存在**。但规格自己在 L230 写着「这一目录尚未建立」 ——
属于**记录在案的降级，不是漂移**。

### 关于「超范围」

复核后确认**几乎都不是** scope creep：

- `media/renderers/`（2 文件/275 行）—— 看着像残留，其实规格树第 47 行
  「`renderers/` 工厂」、`docs/05:58`、`docs/06:820` 都点名它，只是没进 §4 表。
- `media/filters/legacy/` —— 不是越界，是**许可证隔离区**，判据是「算法出处」
  （与 §4.4「按依赖」是另一条判据），§4.2 未记录。
- `media/gl/` —— §4 无小节，但 `docs/02` §6.2 专节定义了它。
- `media/base/composite_demuxer.cc`（378 行）—— 无 §4.1 行，但 `docs/14:309/473`
  明确引入（多源改造 4.9 已闭合），属**有文档的新增**。

---

## 6. 改 `media/` 要跑的门禁

```bash
# 1) 结构不变量（media/ 相关的那几条）
python3 tools/check_invariants.py
#   C1  单文件 ≤500 行（超限需在 LINE_LIMIT_ALLOWLIST 登记并附理由）
#   C4  libav*/libsw* 只许出现在 media/ffmpeg/
#   C5  base/ media/base/ player/public 不得出现平台头与平台宏
#   C16 media/base 不许 include media/filters
#   C22 media/ 与 base/ 不许 include player/
#   C26 media/ 不许 include platform/
#   C27 media/base|filters|renderers 不许 include media/ffmpeg/
#       （media/transcode/ 刻意豁免）
#   C28 render/ 不许 include media/ platform/ base/
#   C23 80 列；C24/C25 源与目标一致

# 2) 风格与静态检查
tools/gate_local.sh --jobs fmt lint

# 3) 单测（按 preset 分三个树）
ctest --test-dir build/<preset> --output-on-failure

# 4) 改了热路径还要跑：微基准 + 端到端 CPU
python3 tools/check_bench_regression.py --baseline tools/bench-baseline.jsonl \
    --current <out.jsonl> --fail-on-regression
python3 tools/measure_e2e_cpu.py

# 5) 任何测量之前：先确认被测二进制是新的
python3 tools/check_binary_freshness.py --root build/<preset>
```

**许可分区是硬约束，不是风格**：`media/filters/legacy/` 下六个文件是 ijkplayer
`ff_ffplay.c` 的逐行移植（LGPL-2.1），其余 `media/` 是 BSD-3-Clause。
往 `legacy/` 里"顺手优化"阈值，等于破坏 golden 测试的前提条件
（阈值必须与原值一致，由 `tools/extract_constants.py` 校验）。

---

## 7. 自测题

1. 一帧数据从 `DataSource` 到 sink，依次经过哪几个类型？各自在哪一层？
2. 为什么 `media/base/` 必须是叶子？如果它 include 了 `media/ffmpeg/`，会破坏什么？
3. C4 和 C27 都在守 FFmpeg 边界，为什么**两条都要**？各自抓什么？
4. `media/transcode/` 可以 include `media/ffmpeg/`，这违反 C27 吗？让 G2 继续成立的是什么？
5. `media/audio/` 和 `seekable_buffer.h` 都不存在 —— 两者的性质有什么**本质区别**？
6. 为什么 `legacy/` 里的魔法数字不能优化？谁在校验它们？
7. `video_frame.h` 的 `CreateBlackFrame` 和 `WrapNativeBuffer` 共同的 6 个参数在提示什么？
8. `ffmpeg_demuxer_configs.cc` 手搓 `extern "C"` 为什么是问题？门禁为什么没抓到？
9. C26 是什么时候才真正成立的？之前是什么状态？

---

## 附 A · 轴一报告：标准符合性

> 以下为子代理报告原文，经轻度清理。**未与轴二合并、未重排。**

### (a) 违反已文档化标准

**1. `media/ffmpeg/ffmpeg_demuxer_configs.cc:7-9` — 硬违规**

```cpp
extern "C" {
#include <libavutil/display.h>
}
```

`media/ffmpeg/av_includes.h:5` 自称 **"THE ONLY PLACE IN AVBASE THAT INCLUDES
FFmpeg HEADERS"**，并列了两项职责：extern "C" 包裹、统一版本开关。这里绕开它自行
include 并手搓 `extern "C"`。C4 只查「位置是否在 `media/ffmpeg/`」，不查「是否
集中」，所以闸门漏掉它。
**初学者该学到什么**：集中化约束的价值在「只有一个入口」，绕过入口就等于废掉约束；
想加新头文件时，先改那个唯一入口，而不是就地复制包裹代码。

**2. `media/base/video_frame.h:186-189` — 硬违规**
`CreateBlackFrame(format, coded_size, natural_size, sar, timestamp, duration,
serial)` = 7 参。STYLE §5：参数 > 6 必须改用 `struct XxxParams`。

**3. `media/base/video_frame.h:198-202` — 硬违规**
`WrapNativeBuffer(...)` = 10 参，同上，且后 4 个（serial/release_cb/to_i420_cb）
本身就是一组「硬件帧生命周期」参数。
**初学者该学到什么**：两个函数共享那 6 个——它们已经在喊着要变成
`VideoFrameParams`；参数列表变长是设计在提示你缺一个类型，不是提示你换行。

**4. `media/renderers/default_renderer_factory.h:58,108` — 判断项**
`Deps` 用 `std::shared_ptr` 持有 sink factory 与 `AvSyncController`。
STYLE §1：`std::shared_ptr` 不推荐，用 `scoped_refptr`。
**初学者该学到什么**：所有权语义要挑工具，不是挑顺手的；跨对象共享且无 weak
需求时应先想 `scoped_refptr`。

**5. `media/ffmpeg/url_data_source.h:75-79` — 判断项**
`// GUARDED_BY(lock_)` 是**注释**而非真注解；`decoder_buffer_queue.h:168` 用的是
真注解。STYLE §7：成员应用 `GUARDED_BY()` 让 `-Wthread-safety` 真正生效——写进
注释等于放弃静态检查。
**初学者该学到什么**：给机器读的注解和给人读的注释不是一回事；能编译期检查的
不要降级成散文。

### (b) 基线味道（均为判断项）

**Duplicated Code — `media/filters/decoder_stream.cc:298-305 / 409-413 / 470-476`**：
三处同一形状「drain `pending_reads_` 并以某 status 交付」，只差 status 来源；
`AdoptSerial:451-454` 与 `Flush:462-465` 又是同一段五行状态复位。
**初学者该学到什么**：抽一个 `DrainPendingReads(status)` 与
`ResetBuffersForNewSerial()`；三份雷同的错误路径就是三处将来只改其中两处的 bug。

**Primitive Obsession — `int32_t serial`**：seek 世代号以裸 `int32_t` 出现在
`video_frame.h:143/188/201`、`decoder_buffer.h:113`、`demuxer_stream.h:70`、
`decoder_stream.cc:315`、`video_renderer_impl.cc:157-160`。它带比较语义
（`serial() != serial_`），是完整领域概念。
**初学者该学到什么**：给「世代号」一个小类型 `class Serial`，比较只能走它的接口；
否则 `serial + 1`、`serial > 5` 这类无意义运算编译得通。

**Message Chains — `video_renderer_impl.cc:157,232`**：
`decoder_stream_.demuxer_stream()->serial()`，VideoRendererImpl 借道 DecoderStream
去问 DemuxerStream。
**初学者该学到什么**：`a.b().c()` 是耦合声明——调用方不该知道这条路径；让
DecoderStream 暴露一个 `current_serial()`。

**Speculative Generality — 抑制**：`renderer.h:160/176/226/239` 四个默认空实现
虚函数看似多余，但每处都写了存在理由（docs/14 多源、rate 0 无法表达暂停）。
仓库有文档背书 → 按规则 (a) 抑制，不报。

### (c) 值得照抄的范例

见正文 §3。

### 两项额外检查

**`media/base/` 是干净的叶子**：对 `media/filters/`、`media/ffmpeg/` 零实际
include（仅注释提及）；平台头与 `__APPLE__`/`__linux__`/`_WIN32` 零命中
（唯一 "wayland-surface" 是 `native_display.cc:18` 的字符串字面量，宏检查器已正确
放过）。**它换来的是**：`media/base` 可被任意宿主编解码器/测试替身引用而无需
拖入 FFmpeg 或平台 ABI，`avbase_media` 的公开头保持零供应商耦合。

**>500 行文件无盲区**：`ffmpeg_demuxer.cc`(928)、`legacy/video_frame_compositor.cc`(700)、
`audio_renderer_algorithm.cc`(664)、`renderer_impl.cc`(598) 全部持有
`LINE_LIMIT_ALLOWLIST` 条目且条目带理由；`legacy/LICENSE.LGPL-2.1`(501) 不是被扫描的
源文件。闸门 522 文件全绿，无需补报。

---

## 附 B · 轴二报告：规格符合性

> 以下为子代理报告原文，经轻度清理。**未与轴一合并、未重排。**

### (a) §4 中缺失或改变落地形态的要求

**§4.1（L155-188）逐项**：见正文 §5 的表。要点：`video_frame_pool` 缺失且无记录；
`video_frame_metadata`、`video_transformation`、`channel_layout`/`sample_format`、
`decoder_config` 系列、`media_log_event` 五项均为**改形态**（内联/合并）而非缺失；
`decoder.h` 与 `seekable_buffer` 缺失且无记录；`cdm_context`/`encryption_pattern`
是有记录的非目标但连占位文件都没建。

**量化**：L188「共约 30 个文件、~4500 行」 vs 实测 **76 文件 / 8726 行**
（2.5× 文件、1.9× 行）。

**§4.2（L192-212）**：`decoder_selector.h/.cc` ✅ 在 `media/filters/`；
`default_decoder_factory.cc` → 化入 `media/ffmpeg/ffmpeg_decoder_factories.*`；
`av_sync_controller.*` 与 ★`video_frame_compositor.*` **在 `media/filters/legacy/`**；
`text_renderer.cc` → 化入 `renderer_impl_text.cc`；`file_data_source.cc` 缺失但有据
（`docs/05:107` ⏳ M12）；`buffered_data_source.cc` → 化入 §4.4 的
`url_data_source.*` + `data_source_io.*`。

**§4.3 `media/audio/`**：目录确实不存在 —— **记录在案的降级**，规格自己在
L230 写明「这一目录尚未建立」，非漂移。

### (b) 规格没要求的行为

`media/renderers/`（2 文件/275 行）**不是残留**，规格树第 47 行与 `docs/05:58`、
`docs/06:820` 都点名它；`media/filters/legacy/` **不是 scope creep**，是许可证
隔离区，README 明写判据是「算法出处」——与 L216「分区判据是依赖」是**另一条判据**，
§4.2 未记录；`media/gl/` 由 `docs/02` §6.2 专节定义；`composite_demuxer.cc` 由
`docs/14:309/473` 引入。

### (c) 已实现但与规格语义分歧

- **D2（零 FFmpeg 类型）成立**：全树 `libav*` 仅命中 `media/ffmpeg/*`、
  `media/transcode/*`，外加 `media/base` 三处**注释**；C4/C27 都守住。
- **`video_frame.h` 确实暴露 `StorageType`**（`:107-113`），且头部注释直接回指
  规格（`:10-11`）。
- **真漂移在别的文档**：`docs/03:925/1189` 仍用缺 `legacy/` 的旧路径；规格
  §4.2 同样没写这两个 ★ 行住在 LGPL 区。

### (d) 对新人：这一轴教什么

`media/` 与 `docs/02` §4 的贴合度是「**语义层面高、文件层面低**」：职责几乎都
落地了，但约 1/3 的 §4.1 行换成了别的文件名，规模也涨到 1.9 倍 ——
**规格是意图清单，不是文件清单**。差别在于：删掉的东西要么自己写下来
（§4.3「尚未建立」），要么在 `docs/12` §4 或 `docs/01` 记一笔非目标（CDM）；
`seekable_buffer`、`video_frame_pool`、`decoder.h` 三者**没有任何文档交代**，
这才是 drift，前两者不是。

**先读两处**：`media/base/video_frame.h`（头部注释直接引用 `docs/02` §9 D2，
是唯一「代码反向标注规格」的地方，能看清 ↔Chromium/↔ijkplayer 两列怎么落到
一行代码上），以及 `media/filters/legacy/README.md`（展示项目如何**为一次
偏离写清判据与许可证后果** —— 这是本仓库认可的偏离仪式）。

---

## 附 C · 复核记录

按项目约定，子代理报告**逐条复核**。绝大多数属实，3 处订正、1 处新发现。

### 已复核属实

| 项 | 结果 |
|---|---|
| `av_includes.h:5` 自称唯一入口 | ✅ 原文如此 |
| `ffmpeg_demuxer_configs.cc:7-9` 手搓 `extern "C"` | ✅ 属实 |
| 它是 `media/ffmpeg/` 里**唯一**绕过的文件 | ✅ 逐文件确认，其余全部走 `av_includes.h` |
| `CreateBlackFrame` 7 参（`:186-190`）| ✅ 属实 |
| `WrapNativeBuffer` 10 参（`:198-202`）| ✅ 属实 |
| `url_data_source.h:75-79` 的 `GUARDED_BY` 是注释 | ✅ 属实 |
| `decoder_buffer_queue.h:168` 用真注解 | ✅ 属实，对比成立 |
| `decoder_stream.cc` 三处 drain（298/409/470）| ✅ 三处形状一致 |
| AdoptSerial 与 Flush 的五行复位重复 | ✅ 属实 |
| `video_frame_pool` / `seekable_buffer` / `decoder.h` / `media/audio/` 均不存在 | ✅ 全树无此名 |
| `media/base/` 零平台宏、零实际越界 include | ✅ 实测确认 |
| `docs/03:424` 引用不存在的 `decoder.h` | ✅ 属实 |
| `docs/03:925` / `:1189` 路径缺 `legacy/` | ✅ 属实 |
| `docs/06:806` / `docs/07:52` 仍列 `seekable_buffer` | ✅ 属实 |
| 四个 >500 行文件都有带理由的 allowlist 条目 | ✅ 闸门全绿 |

### 订正 1 · `ffmpeg_demuxer_configs.cc` 没有 include 顺序违规

轴一称「本项目块排在 C++ 标准库之前」。实际文件顺序是：
自己的头（`:5`）→ `libav*` 用 `extern "C"` 包（`:7-9`）→ `<cmath>`/`<string>`
（`:11-12`）→ 项目头（`:14`）。**符合 STYLE §3。** 真正的问题只有「绕过
`av_includes.h` 这个唯一入口」一条 —— 但它已经足够严重：C4 只查目录不查入口，
所以这是门禁的盲区。

### 订正 3 · 文档里的旧路径是「刻意保留」，不该当成漂移去修

轴二把 `docs/03:925`（`// media/filters/video_frame_compositor.h`）与
`:1189`（`// media/filters/av_sync_controller.h`）归类为「真漂移」，并建议回填。
**这个判断是反的。** README 第 338-343 行的「路径变更通知」明文规定：这三个
文件移入 `media/filters/legacy/` 后，**docs/01–08 中的旧路径刻意保留不改**，理由是
事后改路径会让"当时决定了什么"失真。

所以正确的处置是：
- 925 / 1189（文件搬走了）→ **保留**，别回填；
- 424（`decoder.h` 从未存在）→ 归入 T8「规格列了、从没建、且无文档交代」；
- `docs/06:806` / `docs/07:52` 的 `seekable_buffer` → 同 T8。

**这一条是本轮最值钱的教训，因为它反直觉**：在一个把"偏离必须写下来"当纪律的
仓库里，「文档与代码不一致」有两种截然相反的成因 —— 一种是**应该修的漂移**，
另一种是**刻意冻结的历史**。不查清就动手，会把历史记录改坏。判据写在 README
那段通知里，不在出错的那个文件旁边。

### 订正 2 · `docs/04:330` / `:511` 不是路径漂移

轴二把这两行列为 `media/filters/video_frame_compositor.h` 一类的旧路径。实际那两行是
数据流图里的 `av_sync_controller_->Flush()` 与
`av_sync_controller_->OnAudioFramesConsumed(...)` —— **成员变量名**，不是文件路径。
路径漂移只发生在 `docs/03` 的三处（424 / 925 / 1189）。

### 复核新发现

`default_renderer_factory.h` 的 `Deps` 结构里，`shared_ptr` 与 `scoped_refptr`
**混用**：sink factory 与 `AvSyncController` 用 `shared_ptr`（`:58,:59,:65,:108`），
而 decoder factory 用 `base::scoped_refptr`（`:54,:56,:58` 附近的
`video_decoder_factories`）。轴一把它归入「判断项」是对的，但值得单独指出：
**同一张依赖注入表里两种所有权词汇**，正是 `base::scoped_refptr` 头文件注释
（三个动词）想要消灭的那类歧义。

---

## 附 D · 与 `LEARNING-base.md` 的关系

两篇是同一套方法的两次应用，读法一致：

| | `base/` | `media/` |
|---|---|---|
| 规模 | 45 文件 / 4069 行 | 214 文件 / ~34,600 行 |
| 规格来源 | `docs/02` §3 | `docs/02` §4 |
| 核心不变量 | C1/C5/C22/C23/C24/C25 | **C4/C16/C26/C27/C28**（+ 同上）|
| 最实质问题 | `task_runner_util.h` 注释与代码矛盾 | `ffmpeg_demuxer_configs.cc` 绕过隔离区入口 |
| 共同发现 | 文档引用的门禁编号已失效 | 文档引用的文件路径已失效 |

**两篇共同的一条主线**：这个项目里，偏差是**合法的**，但必须**写下来**。
`bind.h:7` 写下了降级子集，`legacy/README.md` 写下了许可证判据，
`docs/02` §4.3 写下了「尚未建立」。相对地，`seekable_buffer`、
`video_frame_pool`、`decoder.h` 三个缺失**没有任何交代** ——
这就是"有意降级"与"静默漂移"的分界线，也是新人最该学会区分的东西。

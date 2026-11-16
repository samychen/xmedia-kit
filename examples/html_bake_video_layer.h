// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_EXAMPLES_HTML_BAKE_VIDEO_LAYER_H_
#define XMEDIA_EXAMPLES_HTML_BAKE_VIDEO_LAYER_H_

// html_bake 的 <video> 原生层：从 HTML 里的媒体元素建层、逐帧喂帧。
//
// 为什么需要单独一层：CEF OSR 的 OnPaint 不把 <video> 合成进光栅（实测冻结
// 在同一帧），页面里的 video 只是元数据（窗口、时机、音量）。像素必须由
// FFmpeg 解码后经 GlScene 独立层送出，层垫在元素光栅之下（z=-1），场景
// 背景透明让视频透出。
//
// 这层从 html_bake.cc 拆出来（C1 文件长度），拆的是一条本来就存在的缝：
// 上面这段与 CEF 生命周期、VT 时钟、元素拆纹理、编码封装都不共享状态，
// 输入是「元素快照 + 画布尺寸」，输出是「GlScene 上的层」。

#include "media/base/html_element_meta.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_scene.h"
#include "media/transcode/video_element_source.h"

#include <string>
#include <vector>

namespace xmedia {
namespace examples {

// 一个 <video> 场景对应的原生层。
struct VideoElementLayer {
  VideoElementSource src;
  media::GlScene::LayerId layer = media::GlScene::kNoLayer;
  double start_ms = 0;
  double dur_ms = -1;
  // 源内起点（data-media-start）：片段切割出的后段从源视频该时刻继续。
  double from_ms = 0;
  // 元素 id：逐帧用它在快照里取渲染态（不透明度 / 裁剪矩形）。
  std::string id;
};

// 从元素快照里挑出 <video>，逐条 Open 并建层。
//
// |entries| 是采集端上报的元素元数据；只处理 is_media 且扩展名是视频的
// 条目，其余静默跳过（音频元素由导出侧的混音链路负责，不在这里出画）。
// |page_dir| 是契约页所在目录，用于解析相对的 src（空则只认绝对路径与
// file:// 前缀）。|canvas_w|/|canvas_h| 是输出画布像素尺寸，层矩形按它
// 归一化。
//
// 返回建成的层（按 entries 顺序）。打开失败的素材打印一行 stderr 后跳过：
// 少一条视频好过整片烘焙失败——而且缺素材是用户可修的。
std::vector<VideoElementLayer> BuildVideoLayers(
    media::GlScene* scene,
    const std::vector<media::HtmlElementMetaEntry>& entries,
    const std::string& page_dir, int canvas_w, int canvas_h);

// 逐帧更新：按当前时刻算源内位置、取渲染态（不透明度 / 裁剪）、喂帧。
//
// |t_ms| 是输出时间轴上的时刻（毫秒，从 0 起）。|elements| 是本帧的元素
// 渲染态快照。窗口外、淡到看不见、被裁空这三种情况都隐藏该层——隐藏是
// 精确的（非活动层不被采样），所以整片只在真正有画面的段落付解码成本。
void UpdateVideoLayers(
    media::GlScene* scene, std::vector<VideoElementLayer>* layers,
    const std::vector<media::HtmlElementMetaEntry>& elements, double t_ms,
    int canvas_w, int canvas_h);

}  // namespace examples
}  // namespace xmedia

#endif  // XMEDIA_EXAMPLES_HTML_BAKE_VIDEO_LAYER_H_

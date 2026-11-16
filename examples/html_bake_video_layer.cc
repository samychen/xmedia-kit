// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "examples/html_bake_video_layer.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

#include "base/time/time.h"

namespace xmedia {
namespace examples {
namespace {

// 认得的视频容器扩展名。CEF 的 <video> 也只认这几家，多认反而会在
// 素材打不开时报出更难懂的错误。
bool IsVideoExtension(const std::string& src) {
  const auto dot = src.find_last_of('.');
  if (dot == std::string::npos) {
    return false;
  }
  const std::string ext = src.substr(dot);
  static const char* kVideoExts[] = {".mp4", ".webm", ".mov", ".mkv", ".m4v"};
  for (const char* ve : kVideoExts) {
    if (ext == ve) {
      return true;
    }
  }
  return false;
}

// 元素 src → 素材绝对路径。契约页里的 src 有三种写法：file:// 前缀、
// 绝对路径、相对契约页目录。BGM 那种找不到文件的情况在这里不管——
// 打不开由调用方打印并跳过。
std::string ResolveMediaPath(const std::string& src,
                             const std::string& page_dir) {
  if (src.rfind("file://", 0) == 0) {
    return src.substr(7);
  }
  if (src.rfind("/", 0) == 0) {
    return src;
  }
  return page_dir + "/" + src;
}

// 按元素 id 在快照里找回本帧的渲染态。快照按首次出现序排列，元素数量
// 是页面上有 data-* 标记的那几个，线性找足够（视频场景通常 1–2 个）。
const media::HtmlElementMetaEntry* FindMeta(
    const std::vector<media::HtmlElementMetaEntry>& elements,
    const std::string& id) {
  for (const auto& e : elements) {
    if (e.id == id) {
      return &e;
    }
  }
  return nullptr;
}

}  // namespace

std::vector<VideoElementLayer> BuildVideoLayers(
    media::GlScene* scene,
    const std::vector<media::HtmlElementMetaEntry>& entries,
    const std::string& page_dir, int canvas_w, int canvas_h) {
  std::vector<VideoElementLayer> video_layers;
  if (scene == nullptr || canvas_w <= 0 || canvas_h <= 0) {
    return video_layers;
  }
  for (const auto& e : entries) {
    if (!e.is_media || !e.has_src) {
      continue;
    }
    if (!IsVideoExtension(e.src)) {
      continue;
    }
    VideoElementLayer vl;
    if (!vl.src.Open(ResolveMediaPath(e.src, page_dir))) {
      std::fprintf(stderr, "html_bake: 视频元素打开失败 %s\n",
                   e.src.c_str());
      continue;
    }
    vl.start_ms = static_cast<double>(e.start_ms);
    vl.dur_ms = static_cast<double>(e.duration_ms);
    vl.from_ms = static_cast<double>(e.media_start_ms);
    vl.id = e.id;  // 逐帧按 id 取回渲染态（opacity / clip）
    vl.layer = scene->AddVideoLayer(e.has_z ? e.z : -1);
    if (vl.layer == media::GlScene::kNoLayer) {
      continue;
    }
    scene->SetLayerRect(vl.layer, e.x / static_cast<float>(canvas_w),
                        e.y / static_cast<float>(canvas_h),
                        e.w / static_cast<float>(canvas_w),
                        e.h / static_cast<float>(canvas_h));
    scene->SetLayerVisible(vl.layer, false);
    video_layers.push_back(std::move(vl));
  }
  if (!video_layers.empty()) {
    std::fprintf(stderr, "html_bake: 视频元素原生层 %zu 个\n",
                 video_layers.size());
  }
  return video_layers;
}

void UpdateVideoLayers(
    media::GlScene* scene, std::vector<VideoElementLayer>* layers,
    const std::vector<media::HtmlElementMetaEntry>& elements, double t_ms,
    int canvas_w, int canvas_h) {
  if (scene == nullptr || layers == nullptr) {
    return;
  }
  // 视频元素原生层：FFmpeg 解码当前时刻帧 → kARGB 上传（行拷贝进
  // 64 字节对齐平面）；窗外隐藏。
  for (auto& vl : *layers) {
    // 渲染态：入场 / 转场动画动的是场景容器的 opacity 与 clip-path，
    // 而原生视频层直接按矩形出帧。不跟着这两个量走，视频场景之间的
    // 转场（尤其是切割出来的接缝）在成片里就是硬切——页面光栅淡出
    // 了，画面却原地不动，看上去"转场没反应"。
    const media::HtmlElementMetaEntry* meta = FindMeta(elements, vl.id);
    const double local = (t_ms - vl.start_ms) / 1000.0;
    const bool active =
        local >= 0 && (vl.dur_ms < 0 || local * 1000 < vl.dur_ms);
    float alpha = (meta != nullptr && meta->has_opacity)
                      ? static_cast<float>(meta->opacity)
                      : 1.f;
    // 淡到看不见就整层跳过：既省一次解码，也避免 alpha≈0 的层仍占
    // 一次混合。
    if (!active || alpha <= 0.002f) {
      scene->SetLayerVisible(vl.layer, false);
      continue;
    }
    scene->SetLayerOpacity(vl.layer, alpha);
    if (meta != nullptr && meta->w > 0.f && meta->h > 0.f) {
      // 可见矩形 = 元素矩形 ∩ 祖先 clip-path ∩ 画布。源内取样矩形按
      // 同样的交集裁，而不是把整幅画面挤进交集——擦除 / 滑出是裁切，
      // 压缩会变成变形。
      const float cw = static_cast<float>(canvas_w);
      const float ch = static_cast<float>(canvas_h);
      float vx0 = std::max(meta->x, 0.f);
      float vy0 = std::max(meta->y, 0.f);
      float vx1 = std::min(meta->x + meta->w, cw);
      float vy1 = std::min(meta->y + meta->h, ch);
      if (meta->has_clip) {
        vx0 = std::max(vx0, meta->clip_x);
        vy0 = std::max(vy0, meta->clip_y);
        vx1 = std::min(vx1, meta->clip_x + meta->clip_w);
        vy1 = std::min(vy1, meta->clip_y + meta->clip_h);
      }
      const float vw = vx1 - vx0;
      const float vh = vy1 - vy0;
      if (vw <= 0.5f || vh <= 0.5f) {
        scene->SetLayerVisible(vl.layer, false);  // 已完全移出 / 裁空
        continue;
      }
      scene->SetLayerRect(vl.layer, vx0 / cw, vy0 / ch, vw / cw, vh / ch);
      float sx = std::min(std::max((vx0 - meta->x) / meta->w, 0.f), 1.f);
      float sy = std::min(std::max((vy0 - meta->y) / meta->h, 0.f), 1.f);
      const float sw = std::min(vw / meta->w, 1.f - sx);
      const float sh = std::min(vh / meta->h, 1.f - sy);
      if (sw > 0.f && sh > 0.f) {
        scene->SetLayerSourceRect(vl.layer, sx, sy, sw, sh);
      }
    }
    // 源内起点：切割出的后段（data-media-start > 0）从源视频该时刻
    // 继续播；超出源时长时钉在末帧，避免 EOS 后整层变黑。
    double src_sec = local + vl.from_ms / 1000.0;
    const double src_dur = vl.src.duration();
    if (src_dur > 0 && src_sec > src_dur) {
      src_sec = src_dur;
    }
    if (!vl.src.FrameAt(src_sec)) {
      scene->SetLayerVisible(vl.layer, false);
      continue;
    }
    auto vf = media::VideoFrame::CreateBlackFrame(
        media::VideoFormat::kARGB,
        media::Size{vl.src.width(), vl.src.height()},
        media::Size{vl.src.width(), vl.src.height()}, media::Rational{1, 1},
        base::Milliseconds(0), base::Milliseconds(33), 0);
    const size_t sstride = static_cast<size_t>(vl.src.width()) * 4;
    auto span = vf->mutable_data(media::VideoFrame::kYPlane);
    const auto& bgra = vl.src.bgra();
    for (int ry = 0; ry < vl.src.height(); ++ry) {
      std::memcpy(span.data() + static_cast<size_t>(ry) *
                                    vf->stride(media::VideoFrame::kYPlane),
                  bgra.data() + static_cast<size_t>(ry) * sstride, sstride);
    }
    scene->Upload(vl.layer, *vf);
    scene->SetLayerVisible(vl.layer, true);
  }
}

}  // namespace examples
}  // namespace xmedia

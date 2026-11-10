// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/gl/gl_element_composer.h"

#include <algorithm>
#include <utility>

#include "base/logging.h"

namespace xmedia::media {

GlElementComposer::GlElementComposer(Config config) : config_(config) {}

GlElementComposer::~GlElementComposer() = default;

int GlElementComposer::Sync(GlScene& scene, const VideoFrame* frame,
                            const std::vector<HtmlElementMetaEntry>& entries,
                            int64_t t_ms) {
  // 生效元素 quad（文档序），再 (z, ordinal) 稳定排序 —— z 相同的元素
  // 按 DOM 顺序绘制，与 HTML 的绘制顺序一致。
  std::vector<HtmlElementQuad> quads =
      BuildElementQuads(entries, config_.default_z, t_ms);
  std::stable_sort(quads.begin(), quads.end(),
                   [](const HtmlElementQuad& a, const HtmlElementQuad& b) {
                     if (a.z != b.z) {
                       return a.z < b.z;
                     }
                     return a.ordinal < b.ordinal;
                   });

  // 内容矩形的 GL 像素（MapElementRectToCanvas 的输入，y 向上、y 为底边）。
  const float tx = static_cast<float>(config_.target_w);
  const float ty = static_cast<float>(config_.target_h);
  const int cx = static_cast<int>(config_.content_x * tx);
  const int cy_gl =
      static_cast<int>((1.f - config_.content_y - config_.content_h) * ty);
  const int cw = static_cast<int>(config_.content_w * tx);
  const int ch = static_cast<int>(config_.content_h * ty);

  const int tex_w = frame ? frame->coded_size().width : 0;
  const int tex_h = frame ? frame->coded_size().height : 0;
  const bool frame_changed = frame != nullptr && frame != last_uploaded_;

  // 共享纹理：master 层唯一 Upload（不可见、不绘制），元素层 re-Share。
  if (config_.share_textures && frame != nullptr) {
    if (master_layer_ == GlScene::kNoLayer) {
      master_layer_ = scene.AddVideoLayer(INT32_MIN);  // 绘制序垫底
      if (master_layer_ != GlScene::kNoLayer) {
        scene.SetLayerVisible(master_layer_, false);
      }
    }
    if (master_layer_ != GlScene::kNoLayer &&
        (frame_changed || scene.video_layer(master_layer_)->width() == 0) &&
        !scene.Upload(master_layer_, *frame)) {
      LOG(WARNING) << "element composer: master upload refused";
    }
  }
  GlVideoLayer* master_video =
      config_.share_textures && master_layer_ != GlScene::kNoLayer
          ? scene.video_layer(master_layer_)
          : nullptr;

  std::map<std::string, bool> seen;
  for (const HtmlElementQuad& quad : quads) {
    seen[quad.id] = true;
    const auto it = layers_.find(quad.id);
    const bool is_new = it == layers_.end();
    GlScene::LayerId id = GlScene::kNoLayer;
    if (!is_new) {
      id = it->second;
    } else {
      id = scene.AddVideoLayer(quad.z);
      if (id == GlScene::kNoLayer) {
        LOG(WARNING) << "element composer: scene refused a layer for "
                     << quad.id;
        continue;
      }
      layers_.emplace(quad.id, id);
    }

    // 上传：共享模式下元素层是 master 的纹理视图（无自己的上传），
    // 每次 master 上传后 re-Share 保证指针新鲜；独立模式下新层立即
    // 上传、旧层帧变化时重传。
    if (master_video != nullptr) {
      GlVideoLayer* element_video = scene.video_layer(id);
      if (element_video == nullptr) {
        continue;
      }
      element_video->ShareTexturesFrom(*master_video);
      if (!element_video->ready()) {
        scene.SetLayerVisible(id, false);
        continue;  // master 还没有可绘纹理
      }
    } else if (frame != nullptr && (is_new || frame_changed) &&
               !scene.Upload(id, *frame)) {
      LOG(WARNING) << "element composer: upload refused for " << quad.id;
      // 失败层不能留在默认矩形（整目标）里继续画，藏掉。
      scene.SetLayerVisible(id, false);
      continue;
    }

    // UV 窗口（半 texel 内缩）。v=0 是视图顶边 = 存储帧第一行，与
    // SetLayerSourceRect 的归一化源坐标同约定。
    float uv[4];
    ElementQuadUvWindow(quad.x, quad.y, quad.w, quad.h, tex_w, tex_h, uv);
    scene.SetLayerSourceRect(id, uv[0], uv[1], uv[2] - uv[0], uv[3] - uv[1]);

    // 画布定位：视图像素 rect → 内容矩形 GL 像素 → 归一化目标坐标。
    float rx = 0, ry = 0, rw = 0, rh = 0;
    if (!MapElementRectToCanvas(quad.x, quad.y, quad.w, quad.h, cx, cy_gl, cw,
                                ch, tex_w, tex_h, &rx, &ry, &rw, &rh)) {
      scene.SetLayerVisible(id, false);
      continue;
    }
    scene.SetLayerRect(id, rx / tx, 1.f - (ry + rh) / ty, rw / tx, rh / ty);
    scene.SetLayerZ(id, quad.z);
    scene.SetLayerVisible(id, true);
  }

  // 本 tick 不在生效窗的元素：隐藏层保留（层重建贵于一次 Visible(false)）。
  for (const auto& [id_str, layer] : layers_) {
    if (!seen.count(id_str)) {
      scene.SetLayerVisible(layer, false);
    }
  }

  if (frame != nullptr && frame_changed) {
    last_uploaded_ = frame;
  }
  return static_cast<int>(layers_.size());
}

void GlElementComposer::Reset(GlScene& scene) {
  for (const auto& [id_str, layer] : layers_) {
    scene.RemoveLayer(layer);
  }
  layers_.clear();
  if (master_layer_ != GlScene::kNoLayer) {
    scene.RemoveLayer(master_layer_);
    master_layer_ = GlScene::kNoLayer;
  }
  last_uploaded_ = nullptr;
}

GlScene::LayerId GlElementComposer::layer_of(
    const std::string& element_id) const {
  const auto it = layers_.find(element_id);
  return it == layers_.end() ? GlScene::kNoLayer : it->second;
}

}  // namespace xmedia::media

// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_GL_GL_ELEMENT_COMPOSER_H_
#define XMEDIA_MEDIA_GL_GL_ELEMENT_COMPOSER_H_

// GlElementComposer — 元素拆纹理 GPU 渲染路径（HyperFrames 对齐）。
//
// HTML 层（CEF OSR）带元素元数据时，不再整层画一张 quad，而是每个生效
// 元素一个场景层：同一张 OSR 帧上传进各元素层，SetLayerSourceRect 采该
// 元素的 UV 窗口（ElementQuadUvWindow，半 texel 内缩）、SetLayerRect 按
// MapElementRectToCanvas 定位到内容矩形、SetLayerZ 用 data-z —— 元素与
// 其它场景层（主视频等）在同一 z 序里交错，这是 data-z 语义的承诺。
//
// 与 CEF 解耦：输入是任意 VideoFrame（整层 OSR 图）+ HtmlElementMetaTable，
// 合成 SDK 的宿主、bake 管线、测试都可以直接驱动。
//
// 成本模型：每个元素层持有整层纹理的一份拷贝（GlScene 层不共享纹理），
// 帧不变不重传（指针比较）。元素多、帧大时这是 N×纹理内存；共享纹理 +
// 自定义多 quad 绘制是后续优化，登记不阻塞。
//
// THREADING: 全部 GL 线程（与 GlLayerSource 的 consumer 侧一致）。

#include <map>
#include <string>

#include "base/memory/scoped_refptr.h"
#include "media/base/html_element_meta.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_scene.h"
#include "media/media_export.h"

namespace xmedia::media {

class XMEDIA_MEDIA_EXPORT GlElementComposer {
 public:
  // 整层 OSR 帧画进的内容矩形 + 目标画布。SetLayerRect 是归一化目标
  // 坐标（左上原点），这里同约定。
  struct Config {
    int target_w = 0;
    int target_h = 0;
    float content_x = 0.f, content_y = 0.f;  // 归一化，左上原点
    float content_w = 1.f, content_h = 1.f;
    int default_z = 0;  // 未声明 data-z 的元素用（= HTML 层自己的 z）
    // 共享纹理（默认开）：一个隐藏 master 层持有上传，元素层是其纹理
    // 视图——N 元素 1 份纹理拷贝。关掉则每元素层独立上传（旧行为）。
    bool share_textures = true;
  };

  explicit GlElementComposer(Config config);
  GlElementComposer(const GlElementComposer&) = delete;
  GlElementComposer& operator=(const GlElementComposer&) = delete;
  ~GlElementComposer();

  // 同步文档时刻 |t_ms| 的元素层。|frame| 是整层 OSR 帧；与上次不同的
  // 指针触发各元素层的重传（同指针 = 像素未变，跳过）。|entries| 为空时
  // 全部元素层隐藏。返回管理的层数（含隐藏的）。
  int Sync(GlScene& scene, const VideoFrame* frame,
           const std::vector<HtmlElementMetaEntry>& entries, int64_t t_ms);

  // 移除全部元素层（层注销时调用一次）。
  void Reset(GlScene& scene);

  size_t layer_count() const { return layers_.size(); }
  // 测试/诊断：元素 id → 层句柄。不存在返回 kNoLayer。
  GlScene::LayerId layer_of(const std::string& element_id) const;

 private:
  const Config config_;
  // 元素 id → 场景层。消失的元素隐藏层而不是移除：页面循环里同一元素
  // 反复进出生效窗，重建层的纹理分配比一次 SetLayerVisible 贵得多。
  std::map<std::string, GlScene::LayerId> layers_;
  // 共享纹理模式的 master：唯一 Upload 的层（绘制期不可见）；元素层是
  // 它的纹理视图，每次 master 上传后 re-Share（纹理对象可能被重建）。
  GlScene::LayerId master_layer_{GlScene::kNoLayer};
  const VideoFrame* last_uploaded_ = nullptr;  // GL 线程内比较，不做同步
};

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_GL_GL_ELEMENT_COMPOSER_H_

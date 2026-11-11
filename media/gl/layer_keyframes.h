// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_GL_LAYER_KEYFRAMES_H_
#define XMEDIA_MEDIA_GL_LAYER_KEYFRAMES_H_

// 逐属性关键帧 + 缓动预设（MC layer_keyframes 对齐移植）。
//
// 每组关键帧是「时间-值对」数组：t 在区间外钳到端点、区间内按「区段首
// 关键帧的 ease」插值；缓动为 CSS 具名预设或自定义 cubic-bezier（同
// WebKit UnitBezier：x(t) Newton-Raphson 反解 + 二分兜底，y 可越界）。
//
// 求值时钟由调用方决定（离线 = bake seek/VT 时钟；实时 = 墙钟）——同一
// 帧号得到同一值（确定性）。纯逻辑、无 GL 依赖（求值结果由 GlElement
// Composer 应用到场景层）。
//
// 与 MC 的差别：JSON 解析（scene "animation" 对象）未移植——xmedia 的
// 消费方（GlElementComposer::Config）直接接受结构体；场景加载器出现时
// 再补解析层。

#include <cstdint>
#include <vector>

#include "media/media_export.h"

namespace xmedia {
namespace gl {

// 缓动预设。EaseIn/Out/InOut 是固定控制点的 cubic-bezier（对齐 CSS 缓动），
// CubicBezier 用 PropertyKeyframe::bezier 里的自定义控制点。
enum class XMEDIA_MEDIA_EXPORT EaseType {
  Linear,       // linear
  EaseIn,       // ease_in     = cubic-bezier(0.42, 0, 1, 1)
  EaseOut,      // ease_out    = cubic-bezier(0, 0, 0.58, 1)
  EaseInOut,    // ease_in_out = cubic-bezier(0.42, 0, 0.58, 1)
  CubicBezier,  // cubic-bezier(x1,y1,x2,y2)，控制点存 keyframe.bezier
};

// 单个关键帧：ease 作用于「本关键帧 → 下一关键帧」区段（同 CSS 每关键帧
// animation-timing-function 语义）；未指定为 Linear。
struct XMEDIA_MEDIA_EXPORT PropertyKeyframe {
  int64_t t_ms = 0;
  float value = 0.f;
  EaseType ease = EaseType::Linear;
  float bezier[4] = {0.25f, 0.1f, 0.25f, 1.f};  // CubicBezier: x1,y1,x2,y2
};

// 一个图层的逐属性动画。语义（对齐 MC SourceTransform 静态字段）：
// - x / y        ：画布归一化坐标（左上原点），绝对值替换静态 x/y；
// - scale        ：缩放倍率（1 = 静态尺寸），绕内容中心缩放；
// - opacity      ：0..1，替换静态 opacity。
// 空数组 = 该属性无动画（用静态值）。rotation_deg 在 MC 里是整层旋转；
// 元素拆纹理下每元素层只能绕自身中心旋转，与整层语义不等价——不移植，
// 需要时由调用方在场景层自行处理。
struct XMEDIA_MEDIA_EXPORT LayerPropertyAnimation {
  std::vector<PropertyKeyframe> x;
  std::vector<PropertyKeyframe> y;
  std::vector<PropertyKeyframe> scale;
  std::vector<PropertyKeyframe> opacity;

  bool HasAny() const {
    return !x.empty() || !y.empty() || !scale.empty() || !opacity.empty();
  }
};

// 一次性求值结果（无关键帧的属性 has_* = false）。
struct XMEDIA_MEDIA_EXPORT AnimatedLayerValues {
  bool has_x = false;
  float x = 0.f;
  bool has_y = false;
  float y = 0.f;
  bool has_scale = false;
  float scale = 1.f;
  bool has_opacity = false;
  float opacity = 1.f;
};

// 缓动求解：u∈[0,1] → 缓动后的进度。CubicBezier 用 Newton-Raphson + 二分
// 兜底，x∈[0,1] 合法、y 可越界（回弹类曲线）。
float XMEDIA_MEDIA_EXPORT SolveEase(EaseType ease, float u,
                                    const float bezier[4]);

// 关键帧求值：空数组返回 default_value；t 在区间外钳到端点；区间内按
// 「区段首关键帧的 ease」插值。keys 必须按 t_ms 升序（调用方保证）。
float XMEDIA_MEDIA_EXPORT EvaluatePropertyKeyframes(
    const std::vector<PropertyKeyframe>& keys, int64_t t_ms,
    float default_value);

// 一次性求值全部属性（无关键帧的属性 has_* = false）。
void XMEDIA_MEDIA_EXPORT EvaluateLayerAnimation(
    const LayerPropertyAnimation& anim, int64_t t_ms,
    AnimatedLayerValues* out);

}  // namespace gl
}  // namespace xmedia

#endif  // XMEDIA_MEDIA_GL_LAYER_KEYFRAMES_H_

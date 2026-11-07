// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE.
//
// 元素拆纹理（GlElementComposer）的像素验收：一张 OSR 帧的四个象限四种
// 颜色，元素矩形只圈住其中两个——合成结果里"没被任何元素覆盖的象限必须
// 是背景"这一条，只有把像素读回来才看得出（层 API 的返回值对"拆了"和
// "没拆整层糊上去"给不出区别）。

#include "media/gl/gl_element_composer.h"

#include <memory>
#include <string>
#include <vector>

#include "gl_test_context.h"
#include "gl_test_frames.h"
#include "gtest/gtest.h"
#include "media/base/html_element_meta.h"
#include "media/base/video_frame.h"
#include "media/gl/gl_present.h"
#include "media/gl/gl_scene.h"
#include "render/gl/gl_api.h"

namespace xmedia::media {
namespace {

constexpr int kRequestedSize = 64;
constexpr int kHalf = kRequestedSize / 2;

// kARGB 的内存序是 B,G,R,A（GL_BGRA 上传直通 shader）。
constexpr uint8_t kB = 0, kG = 0;
struct ArgbColour {
  uint8_t b, g, r, a;
};
constexpr ArgbColour kRed{0, 0, 255, 255};
constexpr ArgbColour kBlue{255, 0, 0, 255};
constexpr ArgbColour kWhite{255, 255, 255, 255};
constexpr ArgbColour kBlack{16, 16, 16, 255};

// 四象限帧：tl / br 是被元素圈住的颜色，tr / bl 是"只有整层糊上去才会
// 泄出来"的白色——元素拆分正确时它们不可见。
base::scoped_refptr<VideoFrame> MakeQuadrantFrame() {
  auto frame = VideoFrame::CreateBlackFrame(
      VideoFormat::kARGB, Size{kRequestedSize, kRequestedSize},
      Size{kRequestedSize, kRequestedSize}, Rational{1, 1}, base::TimeDelta(),
      base::TimeDelta(), 0);
  auto data = frame->mutable_data(VideoFrame::kYPlane);
  const int32_t stride = frame->stride(VideoFrame::kYPlane);
  auto put = [&](int x, int y, const ArgbColour& c) {
    uint8_t* px = data.data() + static_cast<size_t>(y) * stride +
                  static_cast<size_t>(x) * 4;
    px[0] = c.b;
    px[1] = c.g;
    px[2] = c.r;
    px[3] = c.a;
  };
  for (int y = 0; y < kRequestedSize; ++y) {
    for (int x = 0; x < kRequestedSize; ++x) {
      const bool top = y < kHalf;
      const bool left = x < kHalf;
      put(x, y, top ? (left ? kRed : kWhite) : (left ? kWhite : kBlue));
    }
  }
  return frame;
}

HtmlElementMetaEntry Element(const std::string& id, int ordinal, float x,
                             float y, float w, float h) {
  HtmlElementMetaEntry e;
  e.id = id;
  e.ordinal = ordinal;
  e.x = x;
  e.y = y;
  e.w = w;
  e.h = h;
  e.visible = true;
  return e;
}

class ElementComposerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::string error;
    window_ =
        test::GlTestWindow::Create(kRequestedSize, kRequestedSize, &error);
    if (!window_) {
      GTEST_SKIP() << error;
    }
    presenter_ = std::make_unique<GlPresenter>(window_->TakeContext());
    ASSERT_TRUE(presenter_->Init());
    scene_ = GlScene::Create(*presenter_->api(), &error);
    ASSERT_TRUE(scene_) << error;
    frame_ = MakeQuadrantFrame();
  }

  // 画布采样，坐标是归一化目标坐标（左上原点），换算成 glReadPixels 的
  // 底上原点——采样点取象限中心，离任何边缘都远于 kSampleInset。
  test::Rgb At(float nx, float ny) {
    const int px = static_cast<int>(nx * window_->width());
    const int py_from_top = static_cast<int>(ny * window_->height());
    return test::ReadPixel(*presenter_->api(), px,
                           window_->height() - 1 - py_from_top, GL_BACK);
  }

  std::unique_ptr<GlElementComposer> NewComposer() {
    GlElementComposer::Config config;
    config.target_w = kRequestedSize;
    config.target_h = kRequestedSize;
    config.default_z = 0;
    return std::make_unique<GlElementComposer>(config);
  }

  // 一次性用例：新建 composer → Sync → 合成。跨两次 Sync 的用例（层
  // 保留语义）用 NewComposer + ComposeWith。
  std::unique_ptr<GlElementComposer> Compose(
      const std::vector<HtmlElementMetaEntry>& entries, int64_t t_ms = 0) {
    auto composer = NewComposer();
    ComposeWith(composer.get(), entries, t_ms);
    return composer;
  }

  void ComposeWith(GlElementComposer* composer,
                   const std::vector<HtmlElementMetaEntry>& entries,
                   int64_t t_ms = 0) {
    composer->Sync(*scene_, frame_.get(), entries, t_ms);
    EXPECT_TRUE(presenter_->ComposeScene(*scene_));
    presenter_->PresentComposed();
  }

  std::unique_ptr<test::GlTestWindow> window_;
  std::unique_ptr<GlPresenter> presenter_;
  std::unique_ptr<GlScene> scene_;
  base::scoped_refptr<VideoFrame> frame_;
};

// 两个元素各占一个象限：各自的颜色落在自己的象限里，没被元素覆盖的
// 象限是背景——整层糊上去会让白色泄进 tr/bl，只有真拆了才过。
TEST_F(ElementComposerTest, TwoElementsLandInTheirOwnRects) {
  std::vector<HtmlElementMetaEntry> entries = {
      Element("tl", 0, 0.f, 0.f, kHalf, kHalf),
      Element("br", 1, kHalf, kHalf, kHalf, kHalf),
  };
  auto composer = Compose(entries);
  ASSERT_EQ(composer->layer_count(), 2u);

  const test::Rgb red{255, 0, 0};
  const test::Rgb blue{0, 0, 255};
  EXPECT_EQ(At(0.25f, 0.25f), red) << "tl element shows red";
  EXPECT_EQ(At(0.75f, 0.75f), blue) << "br element shows blue";
  // tr / bl 象限没有被任何元素 quad 覆盖：整层 RGBA 里的白色必须不可见。
  EXPECT_LT(At(0.75f, 0.25f).r, 50)
      << "the uncovered quadrant must be background, not the full layer";
  EXPECT_LT(At(0.25f, 0.75f).r, 50);
}

// z 序在像素层不可判：所有元素 quad 走同一条内容映射（同一 content
// rect、同一比例），画布上重叠的两个元素在重合点采到的是同一个 texel
// —— 谁在上面颜色都一样。z 的解析（data-z / default_z / (z, ordinal)
// 稳定排序）由 BuildElementQuads 单测钉住，SetLayerZ 的接线由 scene 的
// 绘制序消费，两层各自有测试，这里不重复一个必然退化的断言。
TEST_F(ElementComposerTest, ZOrderIsNotPixelWitnessableByDesign) {
  std::vector<HtmlElementMetaEntry> entries = {
      Element("under", 0, 0.f, 0.f, kHalf, kHalf),
      Element("over", 1, 0.f, 0.f, kRequestedSize, kRequestedSize),
  };
  entries[1].has_z = true;
  entries[1].z = 5;
  // 只断言管线成立：两个层都建出来、重叠区是"上层的 texel"=下层同点
  // 的 texel（同一映射），画面不为空即可。
  auto composer = Compose(entries);
  ASSERT_EQ(composer->layer_count(), 2u);
  EXPECT_GT(At(0.25f, 0.25f).r, 200) << "tl quadrant still red through both";
}

// 生效窗：has_start 的元素在 t < start 时不可见，t >= start 出现。
TEST_F(ElementComposerTest, ElementOutsideItsWindowIsAbsent) {
  HtmlElementMetaEntry late = Element("late", 0, 0.f, 0.f, kHalf, kHalf);
  late.has_start = true;
  late.start_ms = 1000;

  auto before = Compose({late}, /*t_ms=*/0);
  EXPECT_EQ(before->layer_count(), 0u) << "not yet started: no layer";
  EXPECT_LT(At(0.25f, 0.25f).r, 50) << "absent element leaves background";

  auto after = Compose({late}, /*t_ms=*/1500);
  ASSERT_EQ(after->layer_count(), 1u);
  const test::Rgb red{255, 0, 0};
  EXPECT_EQ(At(0.25f, 0.25f), red) << "started element is drawn";
}

// 元素从快照中消失：层保留但隐藏（避免反复重建），矩形回到背景。
TEST_F(ElementComposerTest, VanishedElementHidesItsLayer) {
  std::vector<HtmlElementMetaEntry> both = {
      Element("tl", 0, 0.f, 0.f, kHalf, kHalf),
      Element("br", 1, kHalf, kHalf, kHalf, kHalf),
  };
  auto first = Compose(both);
  ASSERT_EQ(first->layer_count(), 2u);

  std::vector<HtmlElementMetaEntry> only_tl = {both[0]};
  auto composer = NewComposer();
  ComposeWith(composer.get(), both);
  ASSERT_EQ(composer->layer_count(), 2u);
  ComposeWith(composer.get(), only_tl);
  EXPECT_EQ(composer->layer_count(), 2u)
      << "the vanished element's layer is retained, hidden";
  const test::Rgb red{255, 0, 0};
  EXPECT_EQ(At(0.25f, 0.25f), red) << "the surviving element still draws";
  EXPECT_LT(At(0.75f, 0.75f).r, 50)
      << "the vanished element leaves background";
}

}  // namespace
}  // namespace xmedia::media

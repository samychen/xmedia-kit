// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// HyperFrames HTML 元素元数据的纯逻辑套件（移植自合成 SDK 的
// render_html_element_meta_test，摘取承重断言）：payload 解析、
// 跨 tick 合并的粘滞语义、生效窗、元素 quad 过滤、UV 窗口与画布
// 映射、导出。

#include "media/base/html_element_meta.h"

#include <cmath>
#include <string>

#include "gtest/gtest.h"

namespace xmedia::media {
namespace {

HtmlElementRect MakeRect(const std::string& id, float x, float y, float w,
                         float h) {
  HtmlElementRect el;
  el.id = id;
  el.x = x;
  el.y = y;
  el.w = w;
  el.h = h;
  el.visible = w > 0 && h > 0;
  return el;
}

// ---- payload 解析 -------------------------------------------------------

TEST(HtmlRectsPayloadTest, ParsesTheDocumentedShape) {
  const std::string payload =
      "{\"t_ms\":1234,\"elements\":["
      "{\"id\":\"banner\",\"tag\":\"div\",\"x\":10,\"y\":20,\"w\":300,"
      "\"h\":50,\"visible\":true,\"start\":100,\"duration\":2000,\"z\":-5}]}";
  HtmlRectsSnapshot snap;
  ASSERT_TRUE(ParseHtmlRectsPayload(payload, &snap));
  EXPECT_EQ(snap.t_ms, 1234);
  ASSERT_EQ(snap.elements.size(), 1u);
  const HtmlElementRect& el = snap.elements[0];
  EXPECT_EQ(el.id, "banner");
  EXPECT_EQ(el.tag, "div");
  EXPECT_FLOAT_EQ(el.x, 10.f);
  EXPECT_FLOAT_EQ(el.w, 300.f);
  EXPECT_TRUE(el.visible);
  EXPECT_TRUE(el.has_start);
  EXPECT_EQ(el.start_ms, 100);
  EXPECT_TRUE(el.has_duration);
  EXPECT_EQ(el.duration_ms, 2000);
  EXPECT_TRUE(el.has_z);
  EXPECT_EQ(el.z, -5);  // data-z 对齐 CSS z-index，可负
  EXPECT_EQ(el.ordinal, 0);
}

TEST(HtmlRectsPayloadTest, EmptySnapshotAndMissingElementsAreLegal) {
  HtmlRectsSnapshot snap;
  EXPECT_TRUE(ParseHtmlRectsPayload("{}", &snap));
  EXPECT_TRUE(snap.elements.empty());
  EXPECT_TRUE(ParseHtmlRectsPayload(
      "{\"t_ms\":5,\"elements\":[]}", &snap));
  EXPECT_EQ(snap.t_ms, 5);
}

TEST(HtmlRectsPayloadTest, StructuralFailuresReject) {
  HtmlRectsSnapshot snap;
  EXPECT_FALSE(ParseHtmlRectsPayload("not json", &snap));
  EXPECT_FALSE(ParseHtmlRectsPayload("[1,2]", &snap));
  // 元素缺必填矩形 / 负宽：单元素非法被"跳过"（快照保留其余元素），
  // 整体解析仍成功——采集脚本侧的坏元素不吞掉整报（原实现语义）。
  EXPECT_TRUE(ParseHtmlRectsPayload(
      "{\"elements\":[{\"id\":\"a\"}]}", &snap));
  EXPECT_TRUE(snap.elements.empty());
  EXPECT_TRUE(ParseHtmlRectsPayload(
      "{\"elements\":[{\"x\":1,\"y\":2,\"w\":-3,\"h\":4},"
      "{\"id\":\"ok\",\"x\":0,\"y\":0,\"w\":1,\"h\":1}]}",
      &snap));
  ASSERT_EQ(snap.elements.size(), 1u);
  EXPECT_EQ(snap.elements[0].id, "ok");
}

TEST(HtmlRectsPayloadTest, IdFallsBackToTagThenOrdinal) {
  HtmlRectsSnapshot snap;
  EXPECT_TRUE(ParseHtmlRectsPayload(
      "{\"elements\":["
      "{\"tag\":\"div\",\"x\":0,\"y\":0,\"w\":1,\"h\":1},"
      "{\"x\":0,\"y\":0,\"w\":1,\"h\":1}]}",
      &snap));
  ASSERT_EQ(snap.elements.size(), 2u);
  EXPECT_EQ(snap.elements[0].id, "div#0");
  EXPECT_EQ(snap.elements[1].id, "element#1");
}

TEST(HtmlRectsPayloadTest, VisibleDefaultsToNonZeroRect) {
  HtmlRectsSnapshot snap;
  EXPECT_TRUE(ParseHtmlRectsPayload(
      "{\"elements\":[{\"id\":\"a\",\"x\":0,\"y\":0,\"w\":10,\"h\":10},"
      "{\"id\":\"b\",\"x\":0,\"y\":0,\"w\":0,\"h\":0}]}",
      &snap));
  EXPECT_TRUE(snap.elements[0].visible);
  EXPECT_FALSE(snap.elements[1].visible);
}

TEST(HtmlRectsPayloadTest, NegativeStartAndDurationClampToZero) {
  HtmlRectsSnapshot snap;
  EXPECT_TRUE(ParseHtmlRectsPayload(
      "{\"elements\":[{\"id\":\"a\",\"x\":0,\"y\":0,\"w\":1,\"h\":1,"
      "\"start\":-5,\"duration\":-9}]}", &snap));
  EXPECT_EQ(snap.elements[0].start_ms, 0);
  EXPECT_EQ(snap.elements[0].duration_ms, 0);
}

TEST(HtmlRectsPayloadTest, UnicodeEscapeSurvivesIntoId) {
  HtmlRectsSnapshot snap;
  EXPECT_TRUE(ParseHtmlRectsPayload(
      "{\"elements\":[{\"id\":\"a\\u4e2db\",\"x\":0,\"y\":0,\"w\":1,"
      "\"h\":1}]}", &snap));
  EXPECT_EQ(snap.elements[0].id, "a\xe4\xb8\xad" "b");  // 中 (UTF-8)
}

// ---- 合并（粘滞语义） ---------------------------------------------------

TEST(HtmlElementMetaTableTest, MergeIsStickyOnTimingLatestOnRect) {
  HtmlElementMetaTable table;
  HtmlRectsSnapshot s1;
  s1.t_ms = 100;
  HtmlElementRect a = MakeRect("a", 10, 20, 30, 40);
  a.has_start = true;
  a.start_ms = 500;
  a.has_duration = true;
  a.duration_ms = 3000;
  a.has_z = true;
  a.z = 7;
  s1.elements.push_back(a);
  table.Merge(s1);

  // 第二个快照：矩形变了，时序声明没有（粘滞不追动态改动）。
  HtmlRectsSnapshot s2;
  s2.t_ms = 200;
  HtmlElementRect a2 = MakeRect("a", 11, 21, 31, 41);
  a2.has_start = true;
  a2.start_ms = 999;  // 不得覆盖首报的 500
  s2.elements.push_back(a2);
  table.Merge(s2);

  ASSERT_EQ(table.entries().size(), 1u);
  const HtmlElementMetaEntry& e = table.entries()[0];
  EXPECT_EQ(e.start_ms, 500);
  EXPECT_EQ(e.duration_ms, 3000);
  EXPECT_EQ(e.z, 7);
  EXPECT_FLOAT_EQ(e.x, 11.f);
  EXPECT_FLOAT_EQ(e.w, 31.f);
  EXPECT_EQ(e.last_t_ms, 200);
  EXPECT_EQ(e.first_t_ms, 100);
  EXPECT_EQ(e.samples, 2);
  EXPECT_EQ(table.last_t_ms(), 200);
}

TEST(HtmlElementMetaTableTest, NewIdsAppendInFirstSeenOrder) {
  HtmlElementMetaTable table;
  HtmlRectsSnapshot s1;
  s1.elements.push_back(MakeRect("b", 0, 0, 1, 1));
  table.Merge(s1);
  HtmlRectsSnapshot s2;
  s2.elements.push_back(MakeRect("a", 0, 0, 1, 1));
  s2.elements.push_back(MakeRect("b", 2, 0, 1, 1));
  table.Merge(s2);
  ASSERT_EQ(table.entries().size(), 2u);
  EXPECT_EQ(table.entries()[0].id, "b");
  EXPECT_EQ(table.entries()[1].id, "a");
}

// ---- 生效窗与 quad -------------------------------------------------------

TEST(HtmlElementWindowTest, PresenceSemanticsCoverTheFourShapes) {
  // 两者都有：左闭右开。
  EXPECT_TRUE(IsElementPresentAt(true, 100, true, 200, 100));
  EXPECT_TRUE(IsElementPresentAt(true, 100, true, 200, 299));
  EXPECT_FALSE(IsElementPresentAt(true, 100, true, 200, 300));
  // 仅 start：出现后不消失。
  EXPECT_TRUE(IsElementPresentAt(true, 100, false, 0, 99999));
  EXPECT_FALSE(IsElementPresentAt(true, 100, false, 0, 99));
  // 仅 duration：[0, duration)。
  EXPECT_TRUE(IsElementPresentAt(false, 0, true, 200, 150));
  EXPECT_FALSE(IsElementPresentAt(false, 0, true, 200, 200));
  // 都没有：常驻。
  EXPECT_TRUE(IsElementPresentAt(false, 0, false, 0, 777));
}

TEST(HtmlElementMetaTableTest, BuildElementQuadsFiltersAndDefaultsZ) {
  HtmlElementMetaTable table;
  HtmlRectsSnapshot snap;
  snap.t_ms = 100;
  HtmlElementRect onscreen = MakeRect("on", 0, 0, 10, 10);
  onscreen.has_start = true;
  onscreen.start_ms = 50;
  onscreen.has_duration = true;
  onscreen.duration_ms = 200;
  onscreen.has_z = true;
  onscreen.z = 15;
  HtmlElementRect gone = MakeRect("gone", 0, 0, 10, 10);
  gone.has_start = true;
  gone.start_ms = 0;
  gone.has_duration = true;
  gone.duration_ms = 50;  // [0,50) 在 t=100 已过
  HtmlElementRect zero = MakeRect("zero", 0, 0, 0, 0);  // 0×0 → 不拆
  snap.elements.push_back(onscreen);
  snap.elements.push_back(gone);
  snap.elements.push_back(zero);
  table.Merge(snap);

  const auto quads = BuildElementQuads(table.entries(), /*default_z=*/5, 100);
  ASSERT_EQ(quads.size(), 1u);
  EXPECT_EQ(quads[0].id, "on");
  EXPECT_EQ(quads[0].z, 15);  // has_z 压过 default_z
}

// ---- UV 窗口与画布映射 ---------------------------------------------------

TEST(HtmlElementGeometryTest, UvWindowIsHalfTexelInset) {
  float uv[4] = {};
  ElementQuadUvWindow(10, 20, 30, 40, 100, 200, uv);
  EXPECT_NEAR(uv[0], 10.5f / 100.f, 1e-6f);
  EXPECT_NEAR(uv[1], 20.5f / 200.f, 1e-6f);
  EXPECT_NEAR(uv[2], 39.5f / 100.f, 1e-6f);
  EXPECT_NEAR(uv[3], 59.5f / 200.f, 1e-6f);
  // 贴边元素：u1 被 1.0 封顶。
  ElementQuadUvWindow(90, 20, 30, 40, 100, 200, uv);
  EXPECT_NEAR(uv[2], 1.0f, 1e-6f);
  // 非法输入 → 全纹理窗口。
  ElementQuadUvWindow(0, 0, 0, 0, 100, 200, uv);
  EXPECT_FLOAT_EQ(uv[0], 0.f);
  EXPECT_FLOAT_EQ(uv[3], 1.f);
}

TEST(HtmlElementGeometryTest, CanvasMapFlipsYAndScalesIndependently) {
  float x = 0, y = 0, w = 0, h = 0;
  // 视图 100x200 → 内容矩形在画布上 50x100，位于 (10, 0)（GL 底边 y=0，
  // 顶边 y=100）。元素 (0,0,100,200) 应铺满内容矩形。
  ASSERT_TRUE(MapElementRectToCanvas(0, 0, 100, 200, 10, 0, 50, 100, 100,
                                     200, &x, &y, &w, &h));
  EXPECT_FLOAT_EQ(x, 10.f);
  EXPECT_FLOAT_EQ(y, 0.f);
  EXPECT_FLOAT_EQ(w, 50.f);
  EXPECT_FLOAT_EQ(h, 100.f);
  // 视图顶部的元素（ey=0）映射到 GL 高处：顶边 GL y = 0+100 - (0+h)*sy。
  ASSERT_TRUE(MapElementRectToCanvas(0, 0, 100, 100, 0, 0, 50, 100, 100,
                                     200, &x, &y, &w, &h));
  EXPECT_FLOAT_EQ(y, 50.f);  // 100 - 100*0.5
  EXPECT_FALSE(MapElementRectToCanvas(0, 0, 0, 0, 0, 0, 50, 100, 100, 200,
                                      &x, &y, &w, &h));
}

// ---- 导出 ----------------------------------------------------------------

TEST(HtmlElementsMetaExportTest, OrdersByZThenOrdinalAndEscapes) {
  HtmlElementMetaTable table;
  HtmlRectsSnapshot snap;
  snap.t_ms = 10;
  HtmlElementRect low = MakeRect("low", 0, 0, 5, 5);
  low.ordinal = 0;
  HtmlElementRect high = MakeRect("hi\"quoted\"", 1, 1, 6, 6);
  high.ordinal = 1;
  high.has_z = true;
  high.z = 9;
  snap.elements.push_back(high);
  snap.elements.push_back(low);
  table.Merge(snap);

  HtmlElementMetaDoc doc;
  doc.html_url = "https://x/a.html";
  doc.mode = "bake";
  doc.bake_clock = "seek";
  doc.view_width = 100;
  doc.view_height = 100;
  doc.report.collect_requested = true;
  doc.report.any_report = true;
  doc.report.entries = table.entries();

  const std::string json = BuildElementsMetaJson(doc);
  // z 升序：low（未声明 z，视为 0）在 hi（z=9，id 含引号转义）之前。
  const size_t low_pos = json.find("\"id\":\"low\"");
  const size_t hi_pos = json.find("hi\\\"quoted");
  ASSERT_NE(hi_pos, std::string::npos);
  EXPECT_LT(low_pos, hi_pos);
  EXPECT_NE(json.find("\"generator\":\"xmedia-kit-html-element-meta\""),
            std::string::npos);
  EXPECT_NE(json.find("\"mode\":\"bake\""), std::string::npos);
}

TEST(HtmlElementsMetaExportTest, PathFollowsTheEncodeOutput) {
  EXPECT_EQ(HtmlElementsMetaJsonPath("/tmp/t2.mp4"),
            "/tmp/t2.html_elements.json");
  EXPECT_EQ(HtmlElementsMetaJsonPath("t2.mp4"), "t2.html_elements.json");
  // 空 stem 的 fallback 名本身带 "html_elements"，与后缀拼接后翻倍——
  // 与原实现的语义一致。
  EXPECT_EQ(HtmlElementsMetaJsonPath(""),
            "html_elements.html_elements.json");
}

}  // namespace
}  // namespace xmedia::media

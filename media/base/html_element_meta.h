// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_BASE_HTML_ELEMENT_META_H_
#define XMEDIA_MEDIA_BASE_HTML_ELEMENT_META_H_

// HTML 元素级合成元数据（HyperFrames 语法对齐）：合成图 = HTML 文档，元素
// 用 data 属性声明时序与层级：
//   <div data-start="1000" data-duration="2000" data-z="30">…</div>
//   （单位毫秒，整数；data-z 对齐 CSS z-index 语义，可为负）
//
// 渲染宿主（CEF OSR / 宿主 WebView / 任何 HTML 后端）注入采集脚本，扫描
// [data-start],[data-duration],[data-z] 元素并 getBoundingClientRect 后
// 按快照上报；本模块把 payload 解析合并为跨 tick 的元素表，导出每元素
// 时序/矩形元数据，并给出生效窗 / 元素 quad / UV 窗口等合成消费形态。
//
// 纯逻辑、无 GL / 无浏览器依赖（移植自合成 SDK framework/render/scene/
// html_element_meta，JSON 子集解析为 xmedia-kit 自研，无 jsoncpp 依赖）。
// 线程安全由持有方负责。

#include <cstdint>
#include <string>
#include <vector>

namespace xmedia::media {

// 一条上报快照内的单元素矩形 + data-* 时序声明。
// x/y/w/h 为视图像素（OSR dsf=1 时 CSS px == 视图像素）；x/y 允许负值
//（部分出画元素），w/h 必须 >= 0。
struct HtmlElementRect {
  std::string id;
  std::string tag;
  float x = 0.f;
  float y = 0.f;
  float w = 0.f;
  float h = 0.f;
  bool visible = false;
  bool has_start = false;
  int64_t start_ms = 0;
  bool has_duration = false;
  int64_t duration_ms = 0;
  bool has_z = false;
  int z = 0;
  // HyperFrames 原版词汇（2026-11 契约对齐）：仅采集/导出，合成语义按
  // 需接入（页内 runtime 已驱动媒体同步/可见性）。
  bool has_end = false;
  int64_t end_ms = 0;         // data-end（显式派生终点）
  bool has_track_index = false;
  int track_index = 0;        // data-track-index（legacy: data-layer）
  bool has_media_start = false;
  int64_t media_start_ms = 0; // data-media-start（媒体元素源内偏移）
  bool has_playback_start = false;
  int64_t playback_start_ms = 0;  // data-playback-start
  bool has_playback_rate = false;
  double playback_rate = 1.0;     // data-playback-rate
  bool has_lottie_src = false;
  std::string lottie_src;         // data-lottie-src
  bool has_composition_id = false;
  std::string composition_id;     // data-composition-id（合成根）
  bool has_src = false;
  std::string src;                // src/data-src（音频或媒体文件）
  bool is_media = false;          // audio/video 标签
  bool has_volume = false;
  double volume = 1.0;            // data-volume（1 = 0dB，0 = 静音，>1 增益）
  bool has_fade_in = false;
  double fade_in = 0;             // data-fade-in（秒，clip 起点线性淡入）
  bool has_fade_out = false;
  double fade_out = 0;            // data-fade-out（秒，clip 终点线性淡出）
  bool has_has_audio = false;
  bool has_audio = false;         // data-has-audio="true"（保留视频自带音轨）
  bool has_composition_src = false;
  std::string composition_src;    // data-composition-src（嵌套合成源）
  bool has_composition_variables = false;
  std::string composition_variables;  // data-composition-variables（声明 JSON）
  bool has_variable_values = false;
  std::string variable_values;    // data-variable-values（实例值 JSON）
  int ordinal = 0;  // 快照内的文档序（querySelectorAll 顺序）
};

// 一次 mc.html.rects 上报（JS 采集脚本 → C++）。
struct HtmlRectsSnapshot {
  int64_t t_ms = 0;  // 采集时的文档时钟（bake seek t / 注入时 0）
  std::vector<HtmlElementRect> elements;
};

// 单快照元素数上限（防御异常页面；超出截断，不判失败）。
constexpr size_t kMaxHtmlRectElements = 4096;

// 解析 `mc.html.rects:` 前缀后的 JSON 文本。
// payload 形如 {"t_ms":1234,"elements":[{"id","tag","x","y","w","h",
//   "start","duration","z","visible"}, ...]}。
// 结构非法（根非对象 / elements 非数组 / 单元素缺 x/y/w/h 或 w|h < 0）
// 返回 false；缺 elements 键或空数组合法（页面无 data-* 元素）。t_ms
// 非法时取 0；start/duration 负值钳 0；z 可负。id 缺省回退
// "<tag>#<ordinal>"（tag 也空 → "element#<ordinal>"）。单元素非法 →
// 跳过，快照其余元素保留。
bool ParseHtmlRectsPayload(const std::string& json_text,
                           HtmlRectsSnapshot* out);

// 单条合并后的元素元数据：data-* 时序声明（粘滞）+ 最近一次矩形。
struct HtmlElementMetaEntry {
  std::string id;
  std::string tag;
  bool has_start = false;
  int64_t start_ms = 0;
  bool has_duration = false;
  int64_t duration_ms = 0;
  bool has_z = false;
  int z = 0;
  bool visible = false;
  float x = 0.f, y = 0.f, w = 0.f, h = 0.f;  // 最近一次矩形（视图像素）
  // HyperFrames 原版词汇（同 HtmlElementRect，粘滞语义同 has_z）。
  bool has_end = false;
  int64_t end_ms = 0;
  bool has_track_index = false;
  int track_index = 0;
  bool has_media_start = false;
  int64_t media_start_ms = 0;
  bool has_playback_start = false;
  int64_t playback_start_ms = 0;
  bool has_playback_rate = false;
  double playback_rate = 1.0;
  bool has_lottie_src = false;
  std::string lottie_src;
  bool has_composition_id = false;
  std::string composition_id;
  // 音频/媒体链路（同 HtmlElementRect，粘滞语义）。
  bool has_src = false;
  std::string src;
  bool is_media = false;
  bool has_volume = false;
  double volume = 1.0;
  bool has_fade_in = false;
  double fade_in = 0;
  bool has_fade_out = false;
  double fade_out = 0;
  bool has_has_audio = false;
  bool has_audio = false;
  bool has_composition_src = false;
  std::string composition_src;
  bool has_composition_variables = false;
  std::string composition_variables;
  bool has_variable_values = false;
  std::string variable_values;
  int64_t last_t_ms = 0;                     // 最近矩形采集时钟
  int64_t first_t_ms = -1;                   // 首次采集时钟（-1 = 从未上报）
  int samples = 0;                           // 收到的快照数
  int ordinal = 0;                           // 首次出现时的文档序
};

// 跨 tick 累积表：同 id 合并；时序声明「首个非缺省胜出」（data 属性静态，
// JS 侧动态改动不追，以首次上报为准保持导出稳定）；矩形/可见性取最新
// 快照。
class HtmlElementMetaTable {
 public:
  void Merge(const HtmlRectsSnapshot& snap);
  const std::vector<HtmlElementMetaEntry>& entries() const { return entries_; }
  int64_t last_t_ms() const { return last_t_ms_; }  // -1 = 从未合并
  bool empty() const { return entries_.empty(); }
  void clear();

 private:
  std::vector<HtmlElementMetaEntry> entries_;  // 首次出现序
  int64_t last_t_ms_ = -1;
};

// 元素级元数据汇总（渲染宿主的收集报告填充）。
struct HtmlElementMetaReport {
  bool collect_requested = false;  // 该层开了收集开关
  bool any_report = false;         // 收到过 >=1 次快照（含空快照）
  int rects_received = 0;          // 收到的快照条数
  int64_t last_report_t_ms = -1;
  std::vector<HtmlElementMetaEntry> entries;  // 首次出现序
};

// 元素生效窗（HyperFrames 语义）：[start, start+duration)。
// 调用方需 start/duration 均已知（has_start && has_duration）。
void HtmlElementWindowMs(int64_t start_ms, int64_t duration_ms,
                         int64_t* out_begin, int64_t* out_end);
// t 是否落在生效窗内（左闭右开）。
bool IsElementActiveAt(int64_t start_ms, int64_t duration_ms, int64_t t_ms);

// 元素在文档时刻 t 是否「在画面上」（超集语义，覆盖未完整声明 data-* 的
// 元素）：无 start 无 duration = 常驻；仅 duration = [0, duration)；
// 仅 start = [start, ∞)；两者都有 = [start, start+duration)。
bool IsElementPresentAt(bool has_start, int64_t start_ms, bool has_duration,
                        int64_t duration_ms, int64_t t_ms);

// 单个待合成元素 quad：rect 为视图像素，z 已按 has_z ? data-z : default_z
// 解析（default_z = HTML 层的 z_order）。
struct HtmlElementQuad {
  std::string id;
  int z = 0;
  int ordinal = 0;          // 文档序；同 z 稳定排序用
  float x = 0.f, y = 0.f;   // 视图像素，左上原点
  float w = 0.f, h = 0.f;
};

// 从合并表构建文档时刻 t 的元素 quad 列表：过滤 = IsElementPresentAt(t)
// 且 rect 非零。输出保持文档序；z 排序由调用方统一稳定排序完成。
std::vector<HtmlElementQuad> BuildElementQuads(
    const std::vector<HtmlElementMetaEntry>& entries, int default_z,
    int64_t t_ms);

// 元素视图像素 rect → 画布 GL 像素 rect（原点左下）。整层纹理按
// letterbox 布局画进 [content_x, content_y, content_w, content_h]（GL
// 坐标，content_y 为内容矩形底边）；元素 rect 按内容矩形的 x/y 独立比例
// 映射（与 GPU 对整层 quad 的拉伸一致）。返回 false 当元素 rect 为空或
// 内容矩形非法。
bool MapElementRectToCanvas(float ex, float ey, float ew, float eh,
                            int content_x, int content_y, int content_w,
                            int content_h, int tex_w, int tex_h, float* out_x,
                            float* out_y, float* out_w, float* out_h);

// 元素 rect → 整层纹理的 UV 窗口 [u0, v0, u1, v1]（半 texel 内缩，防
// GL_LINEAR 采到相邻 texel）。ew/eh <= 0 或纹理非法时写 (0,0,1,1)。
void ElementQuadUvWindow(float ex, float ey, float ew, float eh, int tex_w,
                         int tex_h, float out_uv[4]);

// ---- 导出 ---------------------------------------------------------------

// 导出 JSON 文档文本（键序稳定，供「元素拆纹理 quad 按 z 合成」与外部
// 工具消费）。elements 按 z 升序（未声明 z 视为 0），同 z 按首次出现序。
struct HtmlElementMetaDoc {
  std::string html_url;
  std::string mode;        // "live" | "bake"
  std::string bake_clock;  // "seek" | "virtual_time"（live 为 "-"）
  int view_width = 0;
  int view_height = 0;
  HtmlElementMetaReport report;
};
std::string BuildElementsMetaJson(const HtmlElementMetaDoc& doc);

// CLI 导出文件名：<encode_output 去扩展名>.html_elements.json（同目录；
// stem 为空 → "html_elements.json"）。
std::string HtmlElementsMetaJsonPath(const std::string& encode_output);

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_BASE_HTML_ELEMENT_META_H_

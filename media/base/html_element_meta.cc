// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// HtmlElementMetaTable 的合并、生效窗、元素 quad / UV 窗口 / 画布映射，
// 以及元数据 JSON 导出（自拼键序稳定文本——本树无 JSON 库）。解析在
// html_rects_payload.cc（schema 定向 JSON 子集）。

#include "media/base/html_element_meta.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace xmedia::media {

void HtmlElementMetaTable::Merge(const HtmlRectsSnapshot& snap) {
  // 上报在渲染宿主的事件序列上串行到达，快照本身有序。
  last_t_ms_ = snap.t_ms;
  for (const HtmlElementRect& el : snap.elements) {
    HtmlElementMetaEntry* entry = nullptr;
    for (HtmlElementMetaEntry& e : entries_) {
      if (e.id == el.id) {
        entry = &e;
        break;
      }
    }
    if (!entry) {
      entries_.emplace_back();
      entry = &entries_.back();
      entry->id = el.id;
      entry->ordinal = el.ordinal;
      entry->first_t_ms = snap.t_ms;
    }
    if (!el.tag.empty()) {
      entry->tag = el.tag;
    }
    // 时序声明粘滞：首个非缺省胜出（页面 data 属性静态；动态改动不追）。
    if (el.has_start && !entry->has_start) {
      entry->has_start = true;
      entry->start_ms = el.start_ms;
    }
    if (el.has_duration && !entry->has_duration) {
      entry->has_duration = true;
      entry->duration_ms = el.duration_ms;
    }
    if (el.has_z && !entry->has_z) {
      entry->has_z = true;
      entry->z = el.z;
    }
    // 矩形 / 可见性取最新快照。
    if (el.has_end && !entry->has_end) {
      entry->has_end = true;
      entry->end_ms = el.end_ms;
    }
    if (el.has_track_index && !entry->has_track_index) {
      entry->has_track_index = true;
      entry->track_index = el.track_index;
    }
    if (el.has_media_start && !entry->has_media_start) {
      entry->has_media_start = true;
      entry->media_start_ms = el.media_start_ms;
    }
    if (el.has_playback_start && !entry->has_playback_start) {
      entry->has_playback_start = true;
      entry->playback_start_ms = el.playback_start_ms;
    }
    if (el.has_playback_rate && !entry->has_playback_rate) {
      entry->has_playback_rate = true;
      entry->playback_rate = el.playback_rate;
    }
    if (el.has_lottie_src && !entry->has_lottie_src) {
      entry->has_lottie_src = true;
      entry->lottie_src = el.lottie_src;
    }
    if (el.has_composition_id && !entry->has_composition_id) {
      entry->has_composition_id = true;
      entry->composition_id = el.composition_id;
    }
    // 矩形 / 可见性取最新快照。
    entry->visible = el.visible;
    entry->x = el.x;
    entry->y = el.y;
    entry->w = el.w;
    entry->h = el.h;
    entry->last_t_ms = snap.t_ms;
    entry->samples++;
  }
}

void HtmlElementMetaTable::clear() {
  entries_.clear();
  last_t_ms_ = -1;
}

void HtmlElementWindowMs(int64_t start_ms, int64_t duration_ms,
                         int64_t* out_begin, int64_t* out_end) {
  if (out_begin) {
    *out_begin = start_ms;
  }
  if (out_end) {
    *out_end = start_ms + duration_ms;
  }
}

bool IsElementActiveAt(int64_t start_ms, int64_t duration_ms, int64_t t_ms) {
  // 左闭右开 [start, start+duration)；duration 0 → 永不生效。
  return t_ms >= start_ms && t_ms < start_ms + duration_ms;
}

bool IsElementPresentAt(bool has_start, int64_t start_ms, bool has_duration,
                        int64_t duration_ms, int64_t t_ms) {
  if (has_start && has_duration) {
    return IsElementActiveAt(start_ms, duration_ms, t_ms);
  }
  if (has_start) {
    // data-start 无 duration：出现后不消失。
    return t_ms >= start_ms;
  }
  if (has_duration) {
    // 仅 duration：起点视为 0（窗口 [0, duration)，负 t 视为窗前）。
    return t_ms >= 0 && t_ms < duration_ms;
  }
  return true;  // 无时序声明 = 常驻。
}

std::vector<HtmlElementQuad> BuildElementQuads(
    const std::vector<HtmlElementMetaEntry>& entries, int default_z,
    int64_t t_ms) {
  std::vector<HtmlElementQuad> quads;
  quads.reserve(entries.size());
  for (const HtmlElementMetaEntry& e : entries) {
    if (!IsElementPresentAt(e.has_start, e.start_ms, e.has_duration,
                            e.duration_ms, t_ms)) {
      continue;
    }
    if (!(e.w > 0.f) || !(e.h > 0.f)) {
      continue;  // rect 非零才可拆（display:none → 0×0，同 visible=false）。
    }
    HtmlElementQuad q;
    q.id = e.id;
    q.z = e.has_z ? e.z : default_z;
    q.ordinal = e.ordinal;
    q.x = e.x;
    q.y = e.y;
    q.w = e.w;
    q.h = e.h;
    quads.push_back(std::move(q));
  }
  return quads;
}

bool MapElementRectToCanvas(float ex, float ey, float ew, float eh,
                            int content_x, int content_y, int content_w,
                            int content_h, int tex_w, int tex_h, float* out_x,
                            float* out_y, float* out_w, float* out_h) {
  if (out_x == nullptr || out_y == nullptr || out_w == nullptr ||
      out_h == nullptr) {
    return false;
  }
  if (!(ew > 0.f) || !(eh > 0.f) || content_w <= 0 || content_h <= 0 ||
      tex_w <= 0 || tex_h <= 0) {
    return false;
  }
  // x/y 独立比例：与 GPU 把整层 quad 拉伸进内容矩形的方式一致
  //（letterbox 布局下两者近似相等，偶数取整可差 <=1px）。
  const float sx = static_cast<float>(content_w) / static_cast<float>(tex_w);
  const float sy = static_cast<float>(content_h) / static_cast<float>(tex_h);
  // 视图 y 向下（rect.top）；GL y 向上 → 元素顶边 GL y = 内容顶边 - ey*sy。
  const float content_top = static_cast<float>(content_y + content_h);
  *out_x = static_cast<float>(content_x) + ex * sx;
  *out_y = content_top - (ey + eh) * sy;
  *out_w = ew * sx;
  *out_h = eh * sy;
  return true;
}

void ElementQuadUvWindow(float ex, float ey, float ew, float eh, int tex_w,
                         int tex_h, float out_uv[4]) {
  if (out_uv == nullptr) {
    return;
  }
  if (!(ew > 0.f) || !(eh > 0.f) || tex_w <= 0 || tex_h <= 0) {
    out_uv[0] = 0.f;
    out_uv[1] = 0.f;
    out_uv[2] = 1.f;
    out_uv[3] = 1.f;
    return;
  }
  // 半 texel 内缩：quad 边缘采样正好落在边缘 texel 中心（GL_LINEAR 不再
  // 混入相邻 texel）。v0 = 视图顶边（VAO v=0 在 GL 顶边）。
  const float u0 = (ex + 0.5f) / static_cast<float>(tex_w);
  const float v0 = (ey + 0.5f) / static_cast<float>(tex_h);
  const float u1 = std::min(1.f, (ex + ew - 0.5f) / static_cast<float>(tex_w));
  const float v1 = std::min(1.f, (ey + eh - 0.5f) / static_cast<float>(tex_h));
  out_uv[0] = std::min(u0, u1);
  out_uv[1] = std::min(v0, v1);
  out_uv[2] = u1;
  out_uv[3] = v1;
}

// ---- 导出（键序稳定的自拼 JSON） ---------------------------------------

namespace {

std::string JsonEscape(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (const char c : in) {
    switch (c) {
    case '"': out += "\\\""; break;
    case '\\': out += "\\\\"; break;
    case '\b': out += "\\b"; break;
    case '\f': out += "\\f"; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[8];
        snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out.push_back(c);
      }
    }
  }
  return out;
}

std::string Num(double v) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%.2f", v);
  return buf;
}

}  // namespace

std::string BuildElementsMetaJson(const HtmlElementMetaDoc& doc) {
  std::string root = "{";
  root += "\"version\":1";
  root += ",\"generator\":\"xmedia-kit-html-element-meta\"";
  root += ",\"html_url\":\"" + JsonEscape(doc.html_url) + "\"";
  root += ",\"mode\":\"" + JsonEscape(doc.mode) + "\"";
  root += ",\"bake_clock\":\"" + JsonEscape(doc.bake_clock) + "\"";
  root += ",\"view_width\":" + std::to_string(doc.view_width);
  root += ",\"view_height\":" + std::to_string(doc.view_height);
  root += ",\"last_report_t_ms\":";
  root += std::to_string(doc.report.last_report_t_ms);
  root += ",\"element_count\":" + std::to_string(doc.report.entries.size());

  // 输出排序：z 升序（未声明 z 视为 0），同 z 按首次出现（文档）序。
  std::vector<const HtmlElementMetaEntry*> els;
  els.reserve(doc.report.entries.size());
  for (const HtmlElementMetaEntry& e : doc.report.entries) {
    els.push_back(&e);
  }
  std::stable_sort(els.begin(), els.end(),
                   [](const HtmlElementMetaEntry* a,
                      const HtmlElementMetaEntry* b) {
                     const int za = a->has_z ? a->z : 0;
                     const int zb = b->has_z ? b->z : 0;
                     if (za != zb) {
                       return za < zb;
                     }
                     return a->ordinal < b->ordinal;
                   });

  std::string arr;
  for (const HtmlElementMetaEntry* e : els) {
    if (!arr.empty()) {
      arr += ",";
    }
    std::string node = "{";
    node += "\"id\":\"" + JsonEscape(e->id) + "\"";
    node += ",\"tag\":\"" + JsonEscape(e->tag) + "\"";
    if (e->has_start) {
      node += ",\"start_ms\":" + std::to_string(e->start_ms);
    }
    if (e->has_duration) {
      node += ",\"duration_ms\":" + std::to_string(e->duration_ms);
      int64_t win_begin = 0;
      int64_t win_end = 0;
      HtmlElementWindowMs(e->start_ms, e->duration_ms, &win_begin, &win_end);
      node += ",\"window_ms\":{\"begin\":" + std::to_string(win_begin) +
              ",\"end\":" + std::to_string(win_end) + "}";
      node += ",\"active_at_rect_t\":";
      node += IsElementActiveAt(e->start_ms, e->duration_ms, e->last_t_ms)
                  ? "true"
                  : "false";
    }
    if (e->has_z) {
      node += ",\"z\":" + std::to_string(e->z);
    }
    node += ",\"rect\":{\"x\":" + Num(e->x) + ",\"y\":" + Num(e->y) +
            ",\"w\":" + Num(e->w) + ",\"h\":" + Num(e->h) + "}";
    node += ",\"rect_t_ms\":" + std::to_string(e->last_t_ms);
    node += ",\"first_t_ms\":" + std::to_string(e->first_t_ms);
    node += ",\"samples\":" + std::to_string(e->samples);
    node += ",\"visible\":";
    node += e->visible ? "true" : "false";
    node += "}";
    arr += node;
  }
  root += ",\"elements\":[" + arr + "]";
  root += "}";
  return root;
}

std::string HtmlElementsMetaJsonPath(const std::string& encode_output) {
  // <encode_output 去扩展名>.html_elements.json（同目录；stem 为空 →
  // "html_elements.json"）。手写目录/扩展名剥离：这一处用不到
  // <filesystem> 的其余能力。
  std::string dir;
  std::string name = encode_output;
  const size_t slash = name.find_last_of("/\\");
  if (slash != std::string::npos) {
    dir = name.substr(0, slash + 1);
    name = name.substr(slash + 1);
  }
  // 扩展名 = 名字里（不含目录部分）最后一个点之后的内容；点在首位是
  // 隐藏文件（".mp4" 的 stem 视为空）。
  const size_t dot = name.find_last_of('.');
  if (dot != std::string::npos && dot > 0) {
    name = name.substr(0, dot);
  }
  if (name.empty()) {
    name = "html_elements";
  }
  return dir + name + ".html_elements.json";
}

}  // namespace xmedia::media

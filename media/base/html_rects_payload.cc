// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// ParseHtmlRectsPayload 的实现：payload 的 schema 是固定的、扁平的
// （一个 t_ms 整数 + 一个元素对象数组，每元素 ~10 个标量字段），因此
// 解析器是一个 schema 定向的 JSON 子集读取器——本树没有 jsoncpp，而
// mpd_parser 已确立「受控格式的自研子集解析器」先例。通用 JSON 能力
// （嵌套自由结构）不是目标，解析失败即结构非法。

#include <cmath>
#include <cstdlib>
#include <cstring>

#include "media/base/html_element_meta.h"

namespace xmedia::media {
namespace {

// ---- 极小 JSON 读取器（只覆盖 payload 需要的形状） --------------------

struct JsonCursor {
  const char* p;
  const char* end;

  void SkipWs() {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) {
      ++p;
    }
  }
  bool Peek(char c) {
    SkipWs();
    return p < end && *p == c;
  }
  bool Eat(char c) {
    if (!Peek(c)) {
      return false;
    }
    ++p;
    return true;
  }
  // 字符串（含 \" \\ \/ \b \f \n \r \t 与 \uXXXX 转义；\u 按码位直接编
  // UTF-8，代理对拼合）。失败返回 false。
  bool ReadString(std::string* out) {
    if (!Eat('"')) {
      return false;
    }
    out->clear();
    while (p < end) {
      const unsigned char c = static_cast<unsigned char>(*p++);
      if (c == '"') {
        return true;
      }
      if (c != '\\') {
        out->push_back(static_cast<char>(c));
        continue;
      }
      if (p >= end) {
        return false;
      }
      const char esc = *p++;
      switch (esc) {
      case '"': out->push_back('"'); break;
      case '\\': out->push_back('\\'); break;
      case '/': out->push_back('/'); break;
      case 'b': out->push_back('\b'); break;
      case 'f': out->push_back('\f'); break;
      case 'n': out->push_back('\n'); break;
      case 'r': out->push_back('\r'); break;
      case 't': out->push_back('\t'); break;
      case 'u': {
        if (end - p < 4) {
          return false;
        }
        auto hex4 = [this]() -> int {
          int v = 0;
          for (int i = 0; i < 4; ++i) {
            const char h = p[i];
            v <<= 4;
            if (h >= '0' && h <= '9') {
              v += h - '0';
            } else if (h >= 'a' && h <= 'f') {
              v += h - 'a' + 10;
            } else if (h >= 'A' && h <= 'F') {
              v += h - 'A' + 10;
            } else {
              return -1;
            }
          }
          p += 4;
          return v;
        };
        const int cp = hex4();
        if (cp < 0) {
          return false;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF && end - p >= 6 && p[0] == '\\' &&
            p[1] == 'u') {
          p += 2;
          const int lo = hex4();
          if (lo < 0xDC00 || lo > 0xDFFF) {
            return false;
          }
          const int code = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          out->push_back(static_cast<char>(0xF0 | (code >> 18)));
          out->push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
          out->push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
          out->push_back(static_cast<char>(0x80 | (code & 0x3F)));
          break;
        }
        if (cp < 0x80) {
          out->push_back(static_cast<char>(cp));
        } else if (cp < 0x800) {
          out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
          out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
          out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
          out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
          out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        break;
      }
      default:
        return false;
      }
    }
    return false;  // 未闭合
  }

  // 整数或浮点数的词法读取（不含符号解析外的语法校验）。
  bool ReadNumber(double* out) {
    SkipWs();
    const char* begin = p;
    if (p < end && (*p == '-' || *p == '+')) {
      ++p;
    }
    while (p < end && ((*p >= '0' && *p <= '9') || *p == '.' || *p == 'e' ||
                       *p == 'E' || *p == '-' || *p == '+')) {
      ++p;
    }
    if (p == begin) {
      return false;
    }
    char buf[64];
    const size_t n = static_cast<size_t>(p - begin);
    if (n == 0 || n >= sizeof(buf)) {
      return false;
    }
    memcpy(buf, begin, n);
    buf[n] = '\0';
    char* endp = nullptr;
    const double v = strtod(buf, &endp);
    if (endp != buf + n || !std::isfinite(v)) {
      return false;
    }
    *out = v;
    return true;
  }
};

bool ReadIntField(JsonCursor& c, int64_t* out) {
  double v = 0;
  if (!c.ReadNumber(&v)) {
    return false;
  }
  // 必填整数位：非整数（1.5）或超 int64 视为字段非法。
  if (std::floor(v) != v || v < -9.2e18 || v > 9.2e18) {
    return false;
  }
  *out = static_cast<int64_t>(v);
  return true;
}

bool ReadRectElement(JsonCursor& c, int ordinal, HtmlElementRect* out) {
  if (!c.Eat('{')) {
    return false;
  }
  float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
  bool has_x = false, has_y = false, has_w = false, has_h = false;
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
  bool id_read = false, tag_read = false, visible_read = false;
  bool has_start = false, has_duration = false, has_z = false;
  int64_t start_ms = 0, duration_ms = 0;
  int z = 0;
  bool visible = false;
  std::string id, tag;

  if (c.Eat('}')) {  // 空对象：缺必填矩形
    return false;
  }
  while (true) {
    if (c.Eat('}')) {
      break;
    }
    std::string key;
    if (!c.ReadString(&key) || !c.Eat(':')) {
      return false;
    }
    if (key == "id") {
      id_read = c.ReadString(&id);
      if (!id_read) {
        return false;
      }
    } else if (key == "tag") {
      tag_read = c.ReadString(&tag);
      if (!tag_read) {
        return false;
      }
    } else if (key == "visible") {
      c.SkipWs();
      if (c.Peek('t') || c.Peek('f')) {
        visible = !c.Peek('f');
        const char* kw = visible ? "true" : "false";
        visible = visible ? c.Eat('t') && c.Eat('r') && c.Eat('u') &&
                                c.Eat('e')
                          : c.Eat('f') && c.Eat('a') && c.Eat('l') &&
                                c.Eat('s') && c.Eat('e');
        if (!visible) {
          return false;
        }
        visible_read = true;
      } else {
        return false;
      }
    } else if (key == "x" || key == "y" || key == "w" || key == "h") {
      double v = 0;
      if (!c.ReadNumber(&v)) {
        return false;
      }
      if (key == "x") { x = static_cast<float>(v); has_x = true; }
      else if (key == "y") { y = static_cast<float>(v); has_y = true; }
      else if (key == "w") { w = static_cast<float>(v); has_w = true; }
      else { h = static_cast<float>(v); has_h = true; }
    } else if (key == "start" || key == "duration" || key == "z" ||
               key == "end" || key == "track_index" ||
               key == "media_start" || key == "playback_start") {
      int64_t v = 0;
      if (!ReadIntField(c, &v)) {
        return false;
      }
      if (key == "start") {
        has_start = true;
        start_ms = std::max<int64_t>(0, v);
      } else if (key == "duration") {
        has_duration = true;
        duration_ms = std::max<int64_t>(0, v);
      } else if (key == "end") {
        has_end = true;
        end_ms = std::max<int64_t>(0, v);
      } else if (key == "track_index") {
        has_track_index = true;
        track_index = static_cast<int>(v);
      } else if (key == "media_start") {
        has_media_start = true;
        media_start_ms = std::max<int64_t>(0, v);
      } else {
        has_playback_start = true;
        playback_start_ms = std::max<int64_t>(0, v);
      }
    } else if (key == "playback_rate") {
      double num = 0;
      if (!c.ReadNumber(&num)) {
        return false;
      }
      has_playback_rate = true;
      playback_rate = num;
    } else if (key == "lottie_src" || key == "composition_id") {
      std::string sink;
      if (!c.ReadString(&sink)) {
        return false;
      }
      if (key == "lottie_src") {
        has_lottie_src = true;
        lottie_src = std::move(sink);
      } else {
        has_composition_id = true;
        composition_id = std::move(sink);
      }
    } else {
      // 未知键：跳过值（字符串/数字/bool/数组/对象都吞掉）。
      c.SkipWs();
      if (c.Peek('"')) {
        std::string sink;
        if (!c.ReadString(&sink)) {
          return false;
        }
      } else if (c.Peek('{') || c.Peek('[')) {
        const char open = *c.p;
        const char close = open == '{' ? '}' : ']';
        int depth = 0;
        while (c.p < c.end) {
          const char ch = *c.p++;
          if (ch == '"') {  // 字符串内不数括号
            --c.p;
            std::string sink;
            if (!c.ReadString(&sink)) {
              return false;
            }
            continue;
          }
          if (ch == open) {
            ++depth;
          } else if (ch == close) {
            if (--depth == 0) {
              break;
            }
          }
        }
      } else {
        double sink = 0;
        const char* save = c.p;
        if (!c.ReadNumber(&sink)) {
          c.p = save;
          // true/false/null
          for (const char* kw : {"true", "false", "null"}) {
            const size_t n = strlen(kw);
            if (static_cast<size_t>(c.end - c.p) >= n &&
                memcmp(c.p, kw, n) == 0) {
              c.p += n;
              break;
            }
          }
        }
      }
    }
    if (c.Eat(',')) {
      continue;
    }
    if (c.Eat('}')) {
      break;
    }
    return false;
  }

  if (!has_x || !has_y || !has_w || !has_h || w < 0.f || h < 0.f) {
    return false;
  }
  out->x = x;
  out->y = y;
  out->w = w;
  out->h = h;
  out->ordinal = ordinal;
  if (tag_read) {
    out->tag = tag;
  }
  if (id_read) {
    out->id = id;
  }
  if (out->id.empty()) {
    out->id = out->tag.empty() ? ("element#" + std::to_string(ordinal))
                               : (out->tag + "#" + std::to_string(ordinal));
  }
  out->visible = visible_read ? visible : (w > 0.f && h > 0.f);
  out->has_start = has_start;
  out->start_ms = start_ms;
  out->has_duration = has_duration;
  out->duration_ms = duration_ms;
  out->has_z = has_z;
  out->z = z;
  out->has_end = has_end;
  out->end_ms = end_ms;
  out->has_track_index = has_track_index;
  out->track_index = track_index;
  out->has_media_start = has_media_start;
  out->media_start_ms = media_start_ms;
  out->has_playback_start = has_playback_start;
  out->playback_start_ms = playback_start_ms;
  out->has_playback_rate = has_playback_rate;
  out->playback_rate = playback_rate;
  out->has_lottie_src = has_lottie_src;
  out->lottie_src = std::move(lottie_src);
  out->has_composition_id = has_composition_id;
  out->composition_id = std::move(composition_id);
  return true;
}

}  // namespace

bool ParseHtmlRectsPayload(const std::string& json_text,
                           HtmlRectsSnapshot* out) {
  if (!out) {
    return false;
  }
  *out = HtmlRectsSnapshot{};

  JsonCursor c{json_text.data(), json_text.data() + json_text.size()};
  if (!c.Eat('{')) {
    return false;
  }

  bool saw_elements = false;
  if (c.Eat('}')) {  // 空对象：合法（空快照）
    return true;
  }
  while (true) {
    if (c.Eat('}')) {
      break;
    }
    std::string key;
    if (!c.ReadString(&key) || !c.Eat(':')) {
      return false;
    }
    if (key == "t_ms") {
      int64_t t = 0;
      if (ReadIntField(c, &t)) {
        out->t_ms = std::max<int64_t>(0, t);
      }
      // 类型非法时取 0（原实现的宽容度）：游标已在数字后，快照继续。
    } else if (key == "elements") {
      saw_elements = true;
      if (!c.Eat('[')) {
        return false;
      }
      if (c.Eat(']')) {
        continue;  // 空数组合法
      }
      int ordinal = 0;
      while (true) {
        if (out->elements.size() < kMaxHtmlRectElements) {
          HtmlElementRect el;
          if (ReadRectElement(c, ordinal, &el)) {
            out->elements.push_back(std::move(el));
          }
          // 单元素非法 → 跳过（ordinal 仍前进，保持文档序稳定）。
          ++ordinal;
        } else {
          // 上限截断：吞掉剩余元素对象（保持解析器状态一致）。
          HtmlElementRect sink;
          if (!ReadRectElement(c, ordinal, &sink)) {
            return false;
          }
          ++ordinal;
        }
        if (c.Eat(',')) {
          continue;
        }
        if (c.Eat(']')) {
          break;
        }
        return false;
      }
    } else {
      // 未知顶层键：跳过值。
      c.SkipWs();
      if (c.Peek('"')) {
        std::string sink;
        if (!c.ReadString(&sink)) {
          return false;
        }
      } else {
        double sink = 0;
        if (!c.ReadNumber(&sink)) {
          return false;
        }
      }
    }
    if (c.Eat(',')) {
      continue;
    }
    if (c.Eat('}')) {
      break;
    }
    return false;
  }
  if (!saw_elements) {
    return true;  // 缺 elements 键 = 页面无 data-* 元素，合法
  }
  return true;
}

}  // namespace xmedia::media

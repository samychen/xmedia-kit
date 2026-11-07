// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Frame signature computation, comparison and sidecar JSON serialisation.
// Ported from the composition SDK's frame_signature.{h,cpp}. The JSON
// parse/serialise is self-built (no jsoncpp dependency, mpd_parser precedent).

#include "media/transcode/frame_signature.h"

#include <cmath>

namespace xmedia::media::dedup {
namespace {

inline uint8_t RgbToLuma(uint8_t r, uint8_t g, uint8_t b) {
  return static_cast<uint8_t>((r * 19595 + g * 38470 + b * 7471 + 32768) >> 16);
}

std::string JsonEscape(const std::string& in) {
  std::string out;
  for (const char c : in) {
    if (c == '"') { out += "\\\""; }
    else if (c == '\\') { out += "\\\\"; }
    else { out.push_back(c); }
  }
  return out;
}

}  // namespace

bool ComputeRgbaLumaSignature(const uint8_t* rgba, int stride_bytes, int width,
                              int height, bool bottom_up,
                              FrameVerifyRecord* out) {
  if (out == nullptr || rgba == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  const int stride = stride_bytes > 0 ? stride_bytes : width * 4;
  out->grid.assign(static_cast<size_t>(kFrameVerifyGridW) *
                       kFrameVerifyGridH,
                   0);

  const double scale_x =
      static_cast<double>(width) / kFrameVerifyGridW;
  const double scale_y =
      static_cast<double>(height) / kFrameVerifyGridH;
  for (int gy = 0; gy < kFrameVerifyGridH; ++gy) {
    const int y0 = static_cast<int>(std::floor(gy * scale_y));
    int y1 = static_cast<int>(std::floor((gy + 1) * scale_y));
    if (y1 <= y0) y1 = y0 + 1;
    if (y1 > height) y1 = height;
    for (int gx = 0; gx < kFrameVerifyGridW; ++gx) {
      const int x0 = static_cast<int>(std::floor(gx * scale_x));
      int x1 = static_cast<int>(std::floor((gx + 1) * scale_x));
      if (x1 <= x0) x1 = x0 + 1;
      if (x1 > width) x1 = width;
      uint64_t sum = 0;
      uint64_t count = 0;
      for (int y = y0; y < y1; ++y) {
        const int src_y = bottom_up ? (height - 1 - y) : y;
        const uint8_t* row = rgba + static_cast<size_t>(src_y) * stride;
        for (int x = x0; x < x1; ++x) {
          sum += RgbToLuma(row[x * 4 + 0], row[x * 4 + 1], row[x * 4 + 2]);
          ++count;
        }
      }
      out->grid[static_cast<size_t>(gy) * kFrameVerifyGridW + gx] =
          static_cast<uint8_t>(count > 0 ? (sum + count / 2) / count : 0);
    }
  }
  return true;
}

bool ComputeYuvLumaSignature(const uint8_t* y_plane, int stride, int width,
                             int height, FrameVerifyRecord* out) {
  if (out == nullptr || y_plane == nullptr || width <= 0 || height <= 0) {
    return false;
  }
  out->grid.assign(static_cast<size_t>(kFrameVerifyGridW) *
                       kFrameVerifyGridH,
                   0);

  const double scale_x = static_cast<double>(width) / kFrameVerifyGridW;
  const double scale_y = static_cast<double>(height) / kFrameVerifyGridH;
  for (int gy = 0; gy < kFrameVerifyGridH; ++gy) {
    const int y0 = static_cast<int>(std::floor(gy * scale_y));
    int y1 = static_cast<int>(std::floor((gy + 1) * scale_y));
    if (y1 <= y0) y1 = y0 + 1;
    if (y1 > height) y1 = height;
    for (int gx = 0; gx < kFrameVerifyGridW; ++gx) {
      const int x0 = static_cast<int>(std::floor(gx * scale_x));
      int x1 = static_cast<int>(std::floor((gx + 1) * scale_x));
      if (x1 <= x0) x1 = x0 + 1;
      if (x1 > width) x1 = width;
      uint64_t sum = 0;
      uint64_t count = 0;
      for (int y = y0; y < y1; ++y) {
        const uint8_t* row = y_plane + static_cast<size_t>(y) * stride;
        for (int x = x0; x < x1; ++x) {
          sum += row[x];
          ++count;
        }
      }
      out->grid[static_cast<size_t>(gy) * kFrameVerifyGridW + gx] =
          static_cast<uint8_t>(count > 0 ? (sum + count / 2) / count : 0);
    }
  }
  return true;
}

GridCompareResult CompareFrameVerifyRecords(const FrameVerifyRecord& a,
                                            const FrameVerifyRecord& b,
                                            double tolerance_rms) {
  GridCompareResult result;
  const size_t n = a.grid.size();
  if (n == 0 || n != b.grid.size()) {
    result.rms_diff = 255.0;
    result.max_diff = 255.0;
    result.pass = false;
    return result;
  }
  double sum_sq = 0.0;
  double max_diff = 0.0;
  for (size_t i = 0; i < n; ++i) {
    const int d = static_cast<int>(a.grid[i]) - static_cast<int>(b.grid[i]);
    const double ad = std::abs(static_cast<double>(d));
    sum_sq += static_cast<double>(d) * d;
    if (ad > max_diff) {
      max_diff = ad;
    }
  }
  result.rms_diff = std::sqrt(sum_sq / static_cast<double>(n));
  result.max_diff = max_diff;
  result.pass = result.rms_diff <= tolerance_rms;
  return result;
}

std::string SerializeFrameVerifyRecords(
    int grid_w, int grid_h, double fps, int64_t frame_count,
    const std::vector<FrameVerifyRecord>& records) {
  std::string root = "{";
  root += "\"version\":1";
  root += ",\"grid_w\":" + std::to_string(grid_w);
  root += ",\"grid_h\":" + std::to_string(grid_h);
  char fps_buf[32];
  snprintf(fps_buf, sizeof(fps_buf), "%.2f", fps);
  root += ",\"fps\":" + std::string(fps_buf);
  root += ",\"frame_count\":" + std::to_string(frame_count);
  std::string samples;
  for (const FrameVerifyRecord& rec : records) {
    if (static_cast<int>(rec.grid.size()) != grid_w * grid_h) {
      continue;
    }
    if (!samples.empty()) {
      samples += ",";
    }
    std::string item = "{\"frame\":" + std::to_string(rec.frame_index);
    item += ",\"pts_ms\":" + std::to_string(rec.pts_ms);
    std::string grid;
    for (size_t i = 0; i < rec.grid.size(); ++i) {
      if (i > 0) grid += ",";
      grid += std::to_string(static_cast<int>(rec.grid[i]));
    }
    item += ",\"grid\":[" + grid + "]";
    item += "}";
    samples += item;
  }
  root += ",\"samples\":[" + samples + "]";
  root += "}";
  return root;
}

bool ParseFrameVerifyRecords(const std::string& json_text, int* grid_w,
                             int* grid_h, double* fps, int64_t* frame_count,
                             std::vector<FrameVerifyRecord>* records) {
  // Schema-directed JSON subset parse (same approach as html_rects_payload).
  // Supports: {"grid_w":N,"grid_h":N,"fps":F,"frame_count":N,
  //            "samples":[{"frame":N,"pts_ms":N,"grid":[...]}]}
  if (grid_w == nullptr || grid_h == nullptr || fps == nullptr ||
      frame_count == nullptr || records == nullptr) {
    return false;
  }
  records->clear();

  // A minimal recursive descent would be overkill for the fixed schema;
  // extract values with targeted scans (the JSON is machine-generated).
  auto extract_int = [&json_text](const char* key) -> int64_t {
    const std::string needle = "\"" + std::string(key) + "\":";
    const size_t pos = json_text.find(needle);
    if (pos == std::string::npos) {
      return -1;
    }
    return std::strtoll(json_text.c_str() + pos + needle.size(), nullptr, 10);
  };
  auto extract_double = [&json_text](const char* key) -> double {
    const std::string needle = "\"" + std::string(key) + "\":";
    const size_t pos = json_text.find(needle);
    if (pos == std::string::npos) {
      return 0.0;
    }
    return std::strtod(json_text.c_str() + pos + needle.size(), nullptr);
  };

  *grid_w = static_cast<int>(extract_int("grid_w"));
  *grid_h = static_cast<int>(extract_int("grid_h"));
  if (*grid_w <= 0 || *grid_h <= 0 || *grid_w * *grid_h > (1 << 16)) {
    return false;
  }
  *fps = extract_double("fps");
  *frame_count = extract_int("frame_count");

  // Parse the samples array elements.
  const std::string samples_key = "\"samples\":[";
  const size_t samples_start = json_text.find(samples_key);
  if (samples_start == std::string::npos) {
    return false;
  }
  size_t pos = samples_start + samples_key.size();
  const size_t expect = static_cast<size_t>(*grid_w) * *grid_h;
  while (pos < json_text.size()) {
    const size_t obj_start = json_text.find('{', pos);
    if (obj_start == std::string::npos) {
      break;
    }
    const size_t obj_end = json_text.find('}', obj_start);
    if (obj_end == std::string::npos) {
      break;
    }
    const std::string item = json_text.substr(obj_start, obj_end - obj_start + 1);
    pos = obj_end + 1;

    FrameVerifyRecord rec;
    auto extract_item_int = [&item](const char* key) -> int64_t {
      const std::string needle = "\"" + std::string(key) + "\":";
      const size_t p = item.find(needle);
      if (p == std::string::npos) {
        return -1;
      }
      return std::strtoll(item.c_str() + p + needle.size(), nullptr, 10);
    };
    rec.frame_index = extract_item_int("frame");
    rec.pts_ms = extract_item_int("pts_ms");

    // Grid array: "grid":[v,v,...]
    const std::string grid_key = "\"grid\":[";
    const size_t grid_start = item.find(grid_key);
    if (grid_start == std::string::npos) {
      return false;
    }
    size_t gp = grid_start + grid_key.size();
    while (gp < item.size() && rec.grid.size() < expect) {
      if (item[gp] == ']') break;
      const int val = std::atoi(item.c_str() + gp);
      rec.grid.push_back(static_cast<uint8_t>(
          val < 0 ? 0 : (val > 255 ? 255 : val)));
      while (gp < item.size() && item[gp] != ',' && item[gp] != ']') ++gp;
      if (gp < item.size() && item[gp] == ',') ++gp;
    }
    if (rec.grid.size() != expect) {
      return false;
    }
    records->push_back(std::move(rec));

    // Skip to next object or array end.
    while (pos < json_text.size() && json_text[pos] != '{' &&
           json_text[pos] != ']') {
      ++pos;
    }
    if (pos >= json_text.size() || json_text[pos] == ']') {
      break;
    }
  }
  return true;
}

}  // namespace xmedia::media::dedup

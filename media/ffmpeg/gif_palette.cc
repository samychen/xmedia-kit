// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The median-cut quantizer behind GifFramePalette, ported from the
// composition SDK's GifFramePalette (framework/ffmpeg/encode). See the
// header for the byte-order contracts; everything below is pure logic over
// one RGBA8 frame.

#include "media/ffmpeg/gif_palette.h"

#include <algorithm>
#include <vector>
#include <cstring>

namespace xmedia::media::ffmpeg {
namespace {

constexpr int kGifPaletteSize = 256;
constexpr int kGifLutSize = 32 * 32 * 32;  // 5-5-5 grid

struct GifCell {
  uint16_t key;    // 5-5-5 packed (r << 10 | g << 5 | b)
  uint32_t count;  // pixels in this cell
};

}  // namespace

// The nested Impl; GifCell/kGif* above stay file-local.
struct GifFramePalette::Impl {
  // Median cut: repeatedly split the box holding the most pixels along its
  // widest channel at the count median, until 256 boxes exist.
  void Build(const uint8_t* rgba, int w, int h, int stride) {
    width = w;
    height = h;
    const int pitch = stride > 0 ? stride : w * 4;
    cells.clear();
    memset(hist, 0, sizeof(hist));
    cell_key.assign(static_cast<size_t>(w) * h, 0);

    for (int y = 0; y < h; ++y) {
      const uint8_t* row = rgba + static_cast<size_t>(y) * pitch;
      for (int x = 0; x < w; ++x) {
        const uint16_t key = static_cast<uint16_t>(
            ((row[x * 4 + 0] >> 3) << 10) | ((row[x * 4 + 1] >> 3) << 5) |
            (row[x * 4 + 2] >> 3));
        cell_key[static_cast<size_t>(y) * w + x] = key;
        if (hist[key]++ == 0) {
          cells.push_back(GifCell{key, 0});
        }
      }
    }
    for (GifCell& cell : cells) {
      cell.count = hist[cell.key];
    }

    BuildBoxes();
    FillPalette();
    BuildLut();
  }

  const uint8_t* PaletteBgra() const { return palette; }
  int PaletteCount() const { return palette_count; }

  void WriteIndices(uint8_t* dst, int dst_stride) const {
    for (int y = 0; y < height; ++y) {
      uint8_t* out = dst + static_cast<size_t>(y) * dst_stride;
      const uint16_t* row = cell_key.data() + static_cast<size_t>(y) * width;
      for (int x = 0; x < width; ++x) {
        out[x] = cell_to_index[row[x]];
      }
    }
  }

  int Width() const { return width; }
  int Height() const { return height; }

 private:
  struct Box {
    int begin = 0;  // index range into cells
    int end = 0;
    uint32_t count = 0;
    int index = 0;  // palette ordinal
  };

  void BuildBoxes() {
    boxes.clear();
    if (cells.empty()) {
      return;
    }
    Box root;
    root.begin = 0;
    root.end = static_cast<int>(cells.size());
    root.count = static_cast<uint32_t>(width) * height;
    boxes.push_back(root);

    while (static_cast<int>(boxes.size()) < kGifPaletteSize) {
      // Pick the splittable box holding the most pixels.
      int target = -1;
      uint32_t best = 0;
      for (size_t i = 0; i < boxes.size(); ++i) {
        if (boxes[i].end - boxes[i].begin > 1 && boxes[i].count > best) {
          best = boxes[i].count;
          target = static_cast<int>(i);
        }
      }
      if (target < 0) {
        break;  // one colour per cell: the limit is reached
      }
      Box box = boxes[target];
      const int channel = WidestChannel(box);
      const int shift = channel == 0 ? 10 : (channel == 1 ? 5 : 0);
      auto begin = cells.begin() + box.begin;
      auto mid = cells.begin() + box.begin + (box.end - box.begin) / 2;
      std::nth_element(begin, mid, cells.begin() + box.end,
                       [shift](const GifCell& a, const GifCell& b) {
                         return ((a.key >> shift) & 31) <
                                ((b.key >> shift) & 31);
                       });
      Box left = box;
      left.end = box.begin + (box.end - box.begin) / 2;
      Box right = box;
      right.begin = left.end;
      left.count = 0;
      right.count = 0;
      for (int i = left.begin; i < left.end; ++i) {
        left.count += cells[i].count;
      }
      for (int i = right.begin; i < right.end; ++i) {
        right.count += cells[i].count;
      }
      boxes[target] = left;
      boxes.push_back(right);
    }
  }

  int WidestChannel(const Box& box) const {
    int lo[3] = {31, 31, 31};
    int hi[3] = {0, 0, 0};
    for (int i = box.begin; i < box.end; ++i) {
      const uint16_t key = cells[i].key;
      const int v[3] = {(key >> 10) & 31, (key >> 5) & 31, key & 31};
      for (int c = 0; c < 3; ++c) {
        lo[c] = std::min(lo[c], v[c]);
        hi[c] = std::max(hi[c], v[c]);
      }
    }
    int channel = 0;
    int widest = -1;
    for (int c = 0; c < 3; ++c) {
      if (hi[c] - lo[c] > widest) {
        widest = hi[c] - lo[c];
        channel = c;
      }
    }
    return channel;
  }

  void FillPalette() {
    memset(palette, 0, sizeof(palette));
    palette_count = static_cast<int>(boxes.size());
    for (int i = 0; i < palette_count; ++i) {
      Box& box = boxes[i];
      box.index = i;
      // Weighted average inside the box (expanded to 8 bits per channel).
      uint64_t sum[3] = {0, 0, 0};
      uint64_t total = 0;
      for (int c = box.begin; c < box.end; ++c) {
        const uint16_t key = cells[c].key;
        const int v[3] = {(key >> 10) & 31, (key >> 5) & 31, key & 31};
        for (int ch = 0; ch < 3; ++ch) {
          sum[ch] += static_cast<uint64_t>(v[ch] * 255 / 31) * cells[c].count;
        }
        total += cells[c].count;
      }
      const int r = total ? static_cast<int>(sum[0] / total) : 0;
      const int g = total ? static_cast<int>(sum[1] / total) : 0;
      const int b = total ? static_cast<int>(sum[2] / total) : 0;
      palette[i * 4 + 0] = static_cast<uint8_t>(b);
      palette[i * 4 + 1] = static_cast<uint8_t>(g);
      palette[i * 4 + 2] = static_cast<uint8_t>(r);
      palette[i * 4 + 3] = 255;
    }
    if (palette_count == 0) {
      palette_count = 1;
      palette[3] = 255;
    }
  }

  void BuildLut() {
    // Only the cells this frame actually uses need entries: indices are read
    // per pixel, and a pixel always lands on a non-empty cell.
    memset(cell_to_index, 0, sizeof(cell_to_index));
    for (const Box& box : boxes) {
      for (int c = box.begin; c < box.end; ++c) {
        cell_to_index[cells[c].key] = static_cast<uint8_t>(box.index);
      }
    }
  }

  int width = 0;
  int height = 0;
  uint32_t hist[kGifLutSize] = {};
  std::vector<GifCell> cells;
  std::vector<uint16_t> cell_key;
  std::vector<Box> boxes;
  int palette_count = 0;
  uint8_t palette[kGifPaletteSize * 4] = {};
  uint8_t cell_to_index[kGifLutSize] = {};
};


GifFramePalette::GifFramePalette() : impl_(std::make_unique<Impl>()) {}
GifFramePalette::~GifFramePalette() = default;

void GifFramePalette::Build(const uint8_t* rgba, int width, int height,
                            int stride) {
  impl_->Build(rgba, width, height, stride);
}

const uint8_t* GifFramePalette::palette_bgra() const {
  return impl_->PaletteBgra();
}

int GifFramePalette::palette_count() const {
  return impl_->PaletteCount();
}

void GifFramePalette::WriteIndices(uint8_t* dst, int dst_stride) const {
  impl_->WriteIndices(dst, dst_stride);
}

int GifFramePalette::width() const {
  return impl_->Width();
}

int GifFramePalette::height() const {
  return impl_->Height();
}

}  // namespace xmedia::media::ffmpeg

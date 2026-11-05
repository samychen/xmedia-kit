// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// The median-cut GIF quantizer. The palette byte order is the regression
// this suite exists for: the composition SDK's original bug wrote R,G,B
// where FFmpeg's AVPalette32 wants B,G,R,A — a pure-yellow frame came out
// cyan. Every test here pins one colour end to end through Build().

#include "media/ffmpeg/gif_palette.h"

#include <cstring>
#include <vector>

#include "gtest/gtest.h"

namespace xmedia::media::ffmpeg {
namespace {

// Builds a width*height RGBA8 buffer filled row-major from |pixels|.
std::vector<uint8_t> SolidRgba(int width, int height, uint8_t r, uint8_t g,
                               uint8_t b) {
  std::vector<uint8_t> out(static_cast<size_t>(width) * height * 4);
  for (int i = 0; i < width * height; ++i) {
    out[i * 4 + 0] = r;
    out[i * 4 + 1] = g;
    out[i * 4 + 2] = b;
    out[i * 4 + 3] = 255;
  }
  return out;
}

bool PaletteHasColour(const GifFramePalette& palette, uint8_t r, uint8_t g,
                      uint8_t b) {
  for (int i = 0; i < palette.palette_count(); ++i) {
    const uint8_t* entry = palette.palette_bgra() + i * 4;
    // AVPalette32: B,G,R,A.
    if (entry[0] == b && entry[1] == g && entry[2] == r && entry[3] == 255) {
      return true;
    }
  }
  return false;
}

TEST(GifFramePaletteTest, PureYellowStaysYellowNotCyan) {
  GifFramePalette palette;
  auto rgba = SolidRgba(8, 8, 255, 255, 0);
  palette.Build(rgba.data(), 8, 8);
  EXPECT_EQ(palette.palette_count(), 1);
  EXPECT_TRUE(PaletteHasColour(palette, 255, 255, 0))
      << "yellow (R=255,G=255,B=0) must appear as B=0,G=255,R=255 in "
         "AVPalette32 — the swapped-byte-order bug wrote cyan";
  // And the index map points every pixel at that one entry.
  std::vector<uint8_t> indices(static_cast<size_t>(8) * 8, 255);
  palette.WriteIndices(indices.data(), 8);
  for (uint8_t index : indices) {
    EXPECT_EQ(index, 0);
  }
}

TEST(GifFramePaletteTest, PureRedPutsRInTheThirdSlot) {
  GifFramePalette palette;
  auto rgba = SolidRgba(4, 4, 255, 0, 0);
  palette.Build(rgba.data(), 4, 4);
  EXPECT_EQ(palette.palette_count(), 1);
  const uint8_t* entry = palette.palette_bgra();
  EXPECT_EQ(entry[0], 0);    // B
  EXPECT_EQ(entry[1], 0);    // G
  EXPECT_EQ(entry[2], 255);  // R — the third slot, per AVPalette32
  EXPECT_EQ(entry[3], 255);  // A
}

TEST(GifFramePaletteTest, TwoColoursGetTwoEntriesAndCorrectIndices) {
  // Top row pure red, bottom row pure blue: two cells, two palette entries,
  // and every pixel maps to the entry of its own colour.
  const int kWidth = 4;
  const int kHeight = 4;
  std::vector<uint8_t> rgba(static_cast<size_t>(kWidth) * kHeight * 4);
  for (int y = 0; y < kHeight; ++y) {
    for (int x = 0; x < kWidth; ++x) {
      uint8_t* px = rgba.data() + (static_cast<size_t>(y) * kWidth + x) * 4;
      const bool red = y < kHeight / 2;
      px[0] = red ? 255 : 0;
      px[1] = 0;
      px[2] = red ? 0 : 255;
      px[3] = 255;
    }
  }
  GifFramePalette palette;
  palette.Build(rgba.data(), kWidth, kHeight);
  EXPECT_EQ(palette.palette_count(), 2);
  EXPECT_TRUE(PaletteHasColour(palette, 255, 0, 0));
  EXPECT_TRUE(PaletteHasColour(palette, 0, 0, 255));

  std::vector<uint8_t> indices(static_cast<size_t>(kWidth) * kHeight, 0);
  palette.WriteIndices(indices.data(), kWidth);
  for (int x = 0; x < kWidth; ++x) {
    const uint8_t top = indices[x];
    const uint8_t bottom =
        indices[static_cast<size_t>(kHeight - 1) * kWidth + x];
    EXPECT_NE(top, bottom) << "row " << x << ": red and blue share an index";
  }
}

TEST(GifFramePaletteTest, StrideIsHonoured) {
  // A 4-wide frame carried in an 8-wide-pitch buffer (padding between rows):
  // only the first 4 pixels per row may be read.
  GifFramePalette padded;
  std::vector<uint8_t> padded_buf(4 * 8 * 4, 0);
  for (int y = 0; y < 4; ++y) {
    for (int x = 0; x < 4; ++x) {
      uint8_t* px = padded_buf.data() + (static_cast<size_t>(y) * 8 + x) * 4;
      px[0] = 0;
      px[1] = 255;  // green
      px[2] = 0;
      px[3] = 255;
    }
  }
  padded.Build(padded_buf.data(), 4, 4, 8 * 4);
  EXPECT_EQ(padded.palette_count(), 1);
  EXPECT_TRUE(PaletteHasColour(padded, 0, 255, 0));

  // Same pixels tightly packed must produce the identical palette.
  GifFramePalette tight;
  auto tight_buf = SolidRgba(4, 4, 0, 255, 0);
  tight.Build(tight_buf.data(), 4, 4);
  EXPECT_EQ(tight.palette_count(), padded.palette_count());
  EXPECT_EQ(0, memcmp(padded.palette_bgra(), tight.palette_bgra(),
                      static_cast<size_t>(padded.palette_count()) * 4));
}

}  // namespace
}  // namespace xmedia::media::ffmpeg

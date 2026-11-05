// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FFMPEG_GIF_PALETTE_H_
#define XMEDIA_MEDIA_FFMPEG_GIF_PALETTE_H_

#include <stdint.h>

#include <memory>

namespace xmedia::media::ffmpeg {

// Frame-level 256-colour palette for GIF encoding: a 5-5-5 RGB histogram,
// median-cut to at most 256 boxes (weighted-average colour each), and a
// cell→index LUT for writing PAL8 rows. Ported from the composition SDK's
// GifFramePalette (framework/ffmpeg/encode); the algorithm is unchanged, the
// byte orders below are re-derived for this tree.
//
// WHY A SELF-BUILT QUANTIZER. The "gif" codec advertises only PAL8-family
// inputs and swscale cannot OUTPUT pal8 — the colour reduction has to happen
// before the frame reaches the encoder. FFmpeg's palettegen/paletteuse
// filters need a two-pass graph the streaming transcode chain does not have;
// median cut runs per frame, in place.
//
// BYTE ORDERS (both pinned here; both were real bugs elsewhere):
//  - input |rgba| is byte-ordered R,G,B,A per pixel;
//  - |palette_bgra()| is FFmpeg's AVPalette32 layout — B,G,R,A per entry —
//    ready for AVFrame::data[1] of a PAL8 frame.
class GifFramePalette {
 public:
  GifFramePalette();
  ~GifFramePalette();
  GifFramePalette(const GifFramePalette&) = delete;
  GifFramePalette& operator=(const GifFramePalette&) = delete;

  // Builds the palette + index map for one RGBA8 frame. |stride| is the
  // source row pitch in bytes; 0 means tightly packed (width * 4). A stride
  // smaller than width * 4 is rejected at the caller (structured error).
  void Build(const uint8_t* rgba, int width, int height, int stride = 0);

  // FFmpeg AVPalette32 layout (B,G,R,A), 256 entries.
  const uint8_t* palette_bgra() const;
  // Palette entries actually in use (1..256).
  int palette_count() const;

  // Writes PAL8 indices for the built frame, top-down; |dst_stride| must be
  // >= |width|.
  void WriteIndices(uint8_t* dst, int dst_stride) const;

  int width() const;
  int height() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xmedia::media::ffmpeg

#endif  // XMEDIA_MEDIA_FFMPEG_GIF_PALETTE_H_

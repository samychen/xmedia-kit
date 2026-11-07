// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_TRANSCODE_FRAME_SIGNATURE_H_
#define XMEDIA_MEDIA_TRANSCODE_FRAME_SIGNATURE_H_

// Frame content signature for encode verification (T9b): a 32x18 luma grid
// sampled from the frame. After encoding, the output is decoded back and
// the same function computes signatures for comparison — RMS tolerance
// absorbs lossy-encode differences; injected content errors cause large
// grid deviations → detected and reported with frame number.
//
// Pure logic, no GL/libav dependency. Ported from the composition SDK's
// frame_signature.{h,cpp}.

#include <cstdint>
#include <string>
#include <vector>

namespace xmedia::media::dedup {

constexpr int kFrameVerifyGridW = 32;
constexpr int kFrameVerifyGridH = 18;

struct FrameVerifyRecord {
  int64_t frame_index = 0;
  int64_t pts_ms = -1;
  std::vector<uint8_t> grid;  // grid_w * grid_h values (0..255, top-down)
};

// RGBA frame → 32x18 luma grid (box-average downsample, Rec.601 luma).
// stride <= 0 means tightly packed. bottom_up normalises GL/PBO row order.
bool ComputeRgbaLumaSignature(const uint8_t* rgba, int stride_bytes, int width,
                              int height, bool bottom_up,
                              FrameVerifyRecord* out);

// YUV420P Y plane → same grid (for decode-back verification from
// FFmpegVideoDecoder output). width/height are the luma plane dimensions.
bool ComputeYuvLumaSignature(const uint8_t* y_plane, int stride, int width,
                             int height, FrameVerifyRecord* out);

struct GridCompareResult {
  double rms_diff = 0.0;
  double max_diff = 0.0;
  bool pass = false;
};

GridCompareResult CompareFrameVerifyRecords(const FrameVerifyRecord& a,
                                            const FrameVerifyRecord& b,
                                            double tolerance_rms);

// Sidecar JSON serialisation/parse (schema-stable key order).
std::string SerializeFrameVerifyRecords(int grid_w, int grid_h, double fps,
                                        int64_t frame_count,
                                        const std::vector<FrameVerifyRecord>&
                                            records);
bool ParseFrameVerifyRecords(const std::string& json_text, int* grid_w,
                             int* grid_h, double* fps, int64_t* frame_count,
                             std::vector<FrameVerifyRecord>* records);

}  // namespace xmedia::media::dedup

#endif  // XMEDIA_MEDIA_TRANSCODE_FRAME_SIGNATURE_H_

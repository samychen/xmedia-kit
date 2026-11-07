// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_TRANSCODE_ENCODE_VERIFY_H_
#define XMEDIA_MEDIA_TRANSCODE_ENCODE_VERIFY_H_

// T9b verify-seek: after a transcode, seek the OUTPUT container back to each
// sampled input frame's timestamp, decode one frame, and compare the 32x18
// luma grid against the signature captured from the input during the encode.
// A sane lossy encode keeps the RMS well under the tolerance (solid-content
// grids land ~1-2); a broken seek index, a dropped or shifted frame, or an
// encoder emitting wrong content breaks far above it — and the report names
// the frame. Ported from the composition SDK's encode-verify flow; the
// signature math lives in frame_signature.h.

#include <cstdint>
#include <string>
#include <vector>

#include "media/media_export.h"
#include "media/transcode/frame_signature.h"

namespace xmedia::media::dedup {

struct XMEDIA_MEDIA_EXPORT EncodeVerifyParams {
  // Frames checked, evenly subsampled from |input_records|. 0 = all.
  int sample_count = 32;
  // RMS tolerance over the luma grid. Lossy encodes (quantisation,
  // chroma subsampling) keep identical-content grids near 0-2.
  double tolerance_rms = 8.0;
};

struct XMEDIA_MEDIA_EXPORT EncodeVerifyReport {
  bool passed = false;
  int64_t failed_frame = -1;  // frame_index of the first failing sample.
  double worst_rms = 0.0;
  int samples_checked = 0;
  std::vector<FrameVerifyRecord> output_records;  // decoded-back samples.
};

// Returns false when the output could not be opened/decoded at all (the
// report is left default-initialised); per-sample content mismatches do NOT
// return false — they fill the report and set passed=false.
bool XMEDIA_MEDIA_EXPORT VerifyEncodedOutput(
    const std::string& output_path,
    const std::vector<FrameVerifyRecord>& input_records, double fps,
    const EncodeVerifyParams& params, EncodeVerifyReport* report);

// Sidecar JSON: the decoded-back sample records plus the verdict. Still
// parseable by ParseFrameVerifyRecords (its targeted scans ignore the extra
// "verify" object).
std::string XMEDIA_MEDIA_EXPORT SerializeVerifySidecar(
    double fps, int64_t frame_count,
    const std::vector<FrameVerifyRecord>& output_records,
    const EncodeVerifyReport& report);

}  // namespace xmedia::media::dedup

#endif  // XMEDIA_MEDIA_TRANSCODE_ENCODE_VERIFY_H_

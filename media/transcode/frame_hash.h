// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_TRANSCODE_FRAME_HASH_H_
#define XMEDIA_MEDIA_TRANSCODE_FRAME_HASH_H_

// Frame content 64-bit hash for encode dedup (T9a): pre-encode frame
// equality testing — same content as the previous frame → skip the
// conversion and reuse the previous encoded result. Ported from the
// composition SDK's frame_hash.{h,cpp} (xxHash64-style stripe accumulation
// + double avalanche; no third-party dependency).

#include <cstddef>
#include <cstdint>

namespace xmedia::media::dedup {

// 64-bit content hash of a memory block. |seed| mixes in context (e.g.
// dimensions — same content at a different resolution is a different frame).
// data==nullptr or len==0 returns a pure seed-avalanche value.
uint64_t HashBytes64(const void* data, size_t len, uint64_t seed = 0);

// Row-distance-normalised hash: reads (base + row * stride) width_bytes per
// row. A tight-pitch layout and a padded-pitch layout of the same picture
// hash identically (dedup does not break because of readback alignment);
// padding bytes are never read. Row order is intentionally sensitive —
// flipped frames must not be deduplicated. stride <= 0 means tightly packed.
uint64_t HashImageRows(const void* base, int stride_bytes, int width_bytes,
                       int rows, uint64_t seed = 0);

}  // namespace xmedia::media::dedup

#endif  // XMEDIA_MEDIA_TRANSCODE_FRAME_HASH_H_

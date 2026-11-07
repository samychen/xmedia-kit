// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/transcode/frame_hash.h"

#include <cstring>

namespace xmedia::media::dedup {
namespace {

// xxHash64 prime family (values only scatter multiplication low bits — not
// intended to match official xxHash output).
constexpr uint64_t kPrime1 = 11400714785074694791ULL;
constexpr uint64_t kPrime2 = 14029467366897019727ULL;
constexpr uint64_t kPrime3 = 1609587929392839161ULL;
constexpr uint64_t kPrime4 = 9650029242287828579ULL;
constexpr uint64_t kPrime5 = 2870177450012600261ULL;

inline uint64_t Rotl64(uint64_t v, int s) {
  return (v << s) | (v >> (64 - s));
}

inline uint64_t Round64(uint64_t acc, uint64_t lane) {
  acc += lane * kPrime2;
  acc = Rotl64(acc, 31);
  return acc * kPrime1;
}

inline uint64_t MergeRound(uint64_t acc, uint64_t lane) {
  lane = Round64(0, lane);
  acc ^= lane;
  return acc * kPrime1 + kPrime4;
}

inline uint64_t Read64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));  // endianness-agnostic: session-local compare
  return v;
}

inline uint32_t Read32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

inline uint64_t Avalanche64(uint64_t h) {
  h ^= h >> 33;
  h *= kPrime2;
  h ^= h >> 29;
  h *= kPrime3;
  h ^= h >> 32;
  return h;
}

// Tail (< 32 bytes) mixing: absorb 8/4/1 bytes so any byte change spreads.
inline uint64_t MixTail(uint64_t h, const uint8_t* p, size_t len) {
  while (len >= 8) {
    const uint64_t lane = Read64(p);
    h ^= Round64(0, lane);
    h = Rotl64(h, 27) * kPrime1 + kPrime4;
    p += 8;
    len -= 8;
  }
  if (len >= 4) {
    h ^= static_cast<uint64_t>(Read32(p)) * kPrime1;
    h = Rotl64(h, 23) * kPrime2 + kPrime3;
    p += 4;
    len -= 4;
  }
  while (len > 0) {
    h ^= static_cast<uint64_t>(*p) * kPrime5;
    h = Rotl64(h, 11) * kPrime1;
    ++p;
    --len;
  }
  return h;
}

}  // namespace

uint64_t HashBytes64(const void* data, size_t len, uint64_t seed) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  uint64_t h;
  if (len >= 32) {
    uint64_t a = seed + kPrime1 + kPrime2;
    uint64_t b = seed + kPrime2;
    uint64_t c = seed;
    uint64_t d = seed - kPrime1;
    const size_t stripes = len / 32;
    for (size_t i = 0; i < stripes; ++i) {
      a = Round64(a, Read64(p));
      b = Round64(b, Read64(p + 8));
      c = Round64(c, Read64(p + 16));
      d = Round64(d, Read64(p + 24));
      p += 32;
    }
    h = Rotl64(a, 1) + Rotl64(b, 7) + Rotl64(c, 12) + Rotl64(d, 18);
    h = MergeRound(h, a);
    h = MergeRound(h, b);
    h = MergeRound(h, c);
    h = MergeRound(h, d);
    len -= stripes * 32;
  } else {
    h = seed + kPrime5;
  }
  // Length participates in the mix (different prefixes at different lengths).
  h += static_cast<uint64_t>(p - static_cast<const uint8_t*>(data)) +
       len * kPrime5;
  return Avalanche64(MixTail(h, p, len));
}

uint64_t HashImageRows(const void* base, int stride_bytes, int width_bytes,
                       int rows, uint64_t seed) {
  if (base == nullptr || width_bytes <= 0 || rows <= 0) {
    return Avalanche64(seed ^ (0x9E3779B97F4A7C15ULL +
                               static_cast<uint64_t>(width_bytes) *
                                   1315423911ULL +
                               static_cast<uint64_t>(rows)));
  }
  const int effective_stride =
      stride_bytes > 0 ? stride_bytes : width_bytes;
  // Per-row HashBytes64 then fold: equivalent to streaming the active area
  // as one continuous run; pitch differences (padding) do not affect the
  // result.
  uint64_t h = seed;
  const uint8_t* row = static_cast<const uint8_t*>(base);
  for (int r = 0; r < rows; ++r) {
    const uint64_t row_hash =
        HashBytes64(row + static_cast<size_t>(r) * effective_stride,
                    static_cast<size_t>(width_bytes), kPrime5);
    h = MergeRound(h ^ kPrime1, row_hash);
  }
  return Avalanche64(h ^ (static_cast<uint64_t>(rows) * kPrime3 +
                          static_cast<uint64_t>(width_bytes) * kPrime4));
}

}  // namespace xmedia::media::dedup

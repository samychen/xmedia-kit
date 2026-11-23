// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_TRANSCODE_FLV_RTMP_REASSEMBLER_H_
#define XMEDIA_MEDIA_TRANSCODE_FLV_RTMP_REASSEMBLER_H_

#include <cstddef>
#include <cstdint>
#include <vector>

namespace xmedia::media {

// Reassembles the byte stream the FLV muxer emits into whole FLV tags before
// they are handed to librtmp.
//
// librtmp's RTMP_Write() requires each call to start on a tag boundary and to
// contain complete tags. FFmpeg's AVIO flushes at arbitrary 32 KiB boundaries
// and can split a tag header or body across two writes; librtmp then reports
// zero bytes written, which FFmpeg maps to AVERROR_EOF (the "End of file" that
// killed long RTMP publishes). This class owns the stash and the boundary
// contract as a pure, FFmpeg-independent state machine so the contract can be
// unit-tested with hand-crafted splits, with no network or AVIOContext.
//
// It is header-only on purpose: the only user is ffmpeg_encode_muxer.cc, and
// the tests include it directly without adding a link dependency.
class FlvRtmpReassembler {
 public:
  static constexpr size_t kFlvHeaderBytes = 13;  // 9-byte header + PreviousTagSize0
  static constexpr size_t kTagHeaderBytes = 11;
  static constexpr size_t kPrevTagSizeBytes = 4;
  static constexpr size_t kMaxStashBytes = 16u << 20;

  // Appends |size| bytes from |data|. Returns how many leading bytes of
  // buffer() now form a complete FLV header plus whole tags and can be written
  // to librtmp in one RTMP_Write call. Returns -1 for a malformed (non-FLV)
  // stream or when the stash would exceed kMaxStashBytes.
  int Feed(const uint8_t* data, size_t size) {
    stash_.insert(stash_.end(), data, data + size);
    if (stash_.size() > kMaxStashBytes) {
      return -1;
    }

    size_t off = header_done_ ? 0 : kFlvHeaderBytes;
    if (!header_done_) {
      if (stash_.size() < kFlvHeaderBytes) {
        return 0;  // FLV header itself is still incomplete.
      }
      if (stash_[0] != 'F' || stash_[1] != 'L' || stash_[2] != 'V') {
        return -1;
      }
    }

    size_t complete = 0;
    for (;;) {
      if (stash_.size() - off < kTagHeaderBytes) {
        break;
      }
      const uint8_t* tag = stash_.data() + off;
      const size_t body = (static_cast<size_t>(tag[1]) << 16) |
                          (static_cast<size_t>(tag[2]) << 8) |
                          static_cast<size_t>(tag[3]);
      const size_t tag_size = kTagHeaderBytes + body + kPrevTagSizeBytes;
      if (tag_size > stash_.size() - off) {
        break;  // Partial tag: wait for the next write.
      }
      off += tag_size;
      complete = off;
    }
    if (complete == 0) {
      return 0;
    }
    header_done_ = true;
    return static_cast<int>(complete);
  }

  // Bytes ready to write: buffer().data()[0, ready). Valid until the next
  // Feed()/Consume() call.
  const std::vector<uint8_t>& buffer() const { return stash_; }
  bool header_done() const { return header_done_; }

  // Drops the first |n| bytes after they have been written to librtmp.
  void Consume(size_t n) {
    stash_.erase(stash_.begin(),
                 stash_.begin() + static_cast<std::ptrdiff_t>(n));
  }

 private:
  std::vector<uint8_t> stash_;
  bool header_done_ = false;
};

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_TRANSCODE_FLV_RTMP_REASSEMBLER_H_

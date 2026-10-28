// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The segment-open chain of HlsPlaylistDemuxer, split from the read
// orchestration TU (hls_playlist_demuxer_read.cc) for the C1 line limit.
// The seam: this file owns "turn playlist segment N into an open inner
// FFmpegDemuxer" -- fetch, byterange slice, AES-128 decrypt, probe -- while
// the read TU owns what happens between segments. Everything here runs on
// the media sequence except FetchSegmentOnWorker, per the class comment in
// the header.

#include "media/ffmpeg/hls_playlist_demuxer.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "media/ffmpeg/ffmpeg_demuxer.h"
#include "media/ffmpeg/hls_aes_128.h"
#include "media/ffmpeg/url_data_source.h"

namespace avbase::media {
namespace {

// GETs |url| fully on the calling (worker) thread; aborted or empty fetches
// report failure. The same helper shape the open chain in the main TU uses
// for playlists.
bool FetchWholeUrl(const std::string& url,
                   base::raw_ptr<DataSource>* abort_handle,
                   std::mutex* abort_lock, std::string* out) {
  auto source = base::MakeRefCounted<ffmpeg::UrlDataSource>(url);
  {
    std::lock_guard<std::mutex> scoped(*abort_lock);
    *abort_handle = source.get();
  }
  std::string bytes;
  int64_t offset = 0;
  while (true) {
    char buffer[64 * 1024];
    auto result = source->ReadBlocking(offset, sizeof(buffer),
                                       reinterpret_cast<uint8_t*>(buffer));
    if (!result.has_value() || *result == 0) {
      break;
    }
    bytes.append(buffer, static_cast<size_t>(*result));
    offset += *result;
  }
  {
    std::lock_guard<std::mutex> scoped(*abort_lock);
    *abort_handle = nullptr;
  }
  if (bytes.empty()) {
    return false;
  }
  *out = std::move(bytes);
  return true;
}

std::vector<uint8_t> StringBytes(const std::string& text) {
  return std::vector<uint8_t>(text.begin(), text.end());
}

// "0x"-prefixed 32-hex-digit IV -> 16 raw bytes; empty on anything else
// (the caller then falls back to the sequence-number default IV).
std::string IvFromHex(const std::string& hex) {
  if (hex.size() != 34 || !(hex.starts_with("0x") || hex.starts_with("0X"))) {
    return {};
  }
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  };
  std::string out;
  for (size_t i = 2; i + 1 < hex.size(); i += 2) {
    const int hi = nibble(hex[i]);
    const int lo = nibble(hex[i + 1]);
    if (hi < 0 || lo < 0) {
      return {};
    }
    out.push_back(static_cast<char>((hi << 4) | lo));
  }
  return out;
}

}  // namespace

void HlsPlaylistDemuxer::OpenSegment(size_t index, OpenCompletion completion) {
  open_in_flight_ = true;
  segment_delta_ = base::TimeDelta();
  segment_delta_set_ = false;
  if (index >= playlist_.segments.size()) {
    // Past the end: the natural-EOF shape. Nothing is fetched.
    cursor_ = index;
    media_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&HlsPlaylistDemuxer::OnSegmentOpened,
                       base::Unretained(this), index, std::move(completion),
                       base::unexpected(MediaError::Of(
                           ErrorCode::kSourceEos, "no more segments",
                           "index = " + std::to_string(index),
                           "this is the natural end of the playlist"))));
    return;
  }
  cursor_ = index;
  // The completion waits on the member while the worker fetches;
  // OnSegmentBytesReady and OnInnerSegmentInitialized hand it forward on
  // the media sequence.
  open_completion_ = std::move(completion);
  worker_.task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&HlsPlaylistDemuxer::FetchSegmentOnWorker,
                                base::Unretained(this), index));
}

void HlsPlaylistDemuxer::FetchSegmentOnWorker(size_t index) {
  const hls::Segment segment = playlist_.segments[index];
  std::string body;
  if (!FetchWholeUrl(segment.uri, &active_fetch_, &lock_, &body)) {
    media_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            &HlsPlaylistDemuxer::OnSegmentBytesReady, base::Unretained(this),
            index,
            base::unexpected(MediaError::Of(
                ErrorCode::kSourceReadFailed,
                "the segment could not be fetched",
                "url = " + segment.uri +
                    ", sequence = " + std::to_string(segment.media_sequence),
                "the retry decorator upstream reconnects on transport "
                "errors; this error means the segment is really gone"))));
    return;
  }
  std::vector<uint8_t> bytes = StringBytes(body);
  if (segment.byterange_start >= 0 && segment.byterange_length > 0) {
    const size_t start = static_cast<size_t>(segment.byterange_start);
    const size_t end = std::min(
        bytes.size(), start + static_cast<size_t>(segment.byterange_length));
    if (start < bytes.size()) {
      bytes = std::vector<uint8_t>(bytes.begin() + static_cast<long>(start),
                                   bytes.begin() + static_cast<long>(end));
    }
  }
  if (segment.key.is_aes_128()) {
    // Fetch (or reuse) the 16-byte key, then decrypt in place.
    if (segment.key.uri != cached_key_url_ || cached_key_.empty()) {
      std::string key_text;
      if (!FetchWholeUrl(segment.key.uri, &active_fetch_, &lock_, &key_text) ||
          key_text.size() != 16) {
        media_task_runner_->PostTask(
            FROM_HERE,
            base::BindOnce(
                &HlsPlaylistDemuxer::OnSegmentBytesReady,
                base::Unretained(this), index,
                base::unexpected(MediaError::Of(
                    ErrorCode::kSourceUnsupported,
                    "the aes-128 key could not be fetched",
                    "url = " + segment.key.uri,
                    "check the EXT-X-KEY uri, or inject a key provider "
                    "(cicada-port T5)"))));
        return;
      }
      cached_key_ = StringBytes(key_text);
      cached_key_url_ = segment.key.uri;
    }
    std::string iv = IvFromHex(segment.key.iv_hex);
    if (iv.empty()) {
      // The spec default: the segment's media sequence number.
      iv = hls::Aes128CbcDecrypter::SequenceNumberIv(segment.media_sequence);
    }
    std::string key_bytes(cached_key_.begin(), cached_key_.end());
    auto decrypter = hls::Aes128CbcDecrypter::Create(key_bytes, iv);
    if (decrypter == nullptr) {
      media_task_runner_->PostTask(
          FROM_HERE,
          base::BindOnce(&HlsPlaylistDemuxer::OnSegmentBytesReady,
                         base::Unretained(this), index,
                         base::unexpected(MediaError::Of(
                             ErrorCode::kSourceUnsupported,
                             "the aes-128 decrypter could not start",
                             "iv bytes = " + std::to_string(iv.size()),
                             "check the playlist's EXT-X-KEY attributes"))));
      return;
    }
    auto decrypted = decrypter->Decrypt(bytes);
    if (!decrypted.has_value()) {
      media_task_runner_->PostTask(
          FROM_HERE, base::BindOnce(&HlsPlaylistDemuxer::OnSegmentBytesReady,
                                    base::Unretained(this), index,
                                    base::unexpected(decrypted.error())));
      return;
    }
    bytes = std::move(*decrypted);
  }
  media_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &HlsPlaylistDemuxer::OnSegmentBytesReady, base::Unretained(this),
          index,
          base::expected<std::vector<uint8_t>, MediaError>(std::move(bytes))));
}

void HlsPlaylistDemuxer::OnSegmentBytesReady(
    size_t index,
    base::expected<std::vector<uint8_t>, MediaError> bytes_or_error) {
  if (stopped_.load()) {
    open_completion_ = OpenCompletion();
    return;
  }
  if (!bytes_or_error.has_value()) {
    media_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&HlsPlaylistDemuxer::OnSegmentOpened,
                                  base::Unretained(this), index,
                                  std::move(open_completion_),
                                  base::unexpected(bytes_or_error.error())));
    open_completion_ = OpenCompletion();
    return;
  }
  open_segment_bytes_ = std::move(*bytes_or_error);
  auto inner = std::make_unique<FFmpegDemuxer>(media_log_, limits_);
  auto memory = base::MakeRefCounted<MemoryDataSource>(
      open_segment_bytes_.data(), open_segment_bytes_.size());
  DemuxerOptions inner_options = options_;
  inner_options.reconnect = false;  // the segment is already fully in memory
  inner->Initialize(
      DataSourceDescriptor::FromSource(std::move(memory)), inner_options, host_,
      media_task_runner_,
      base::BindOnce(&HlsPlaylistDemuxer::OnInnerSegmentInitialized,
                     base::Unretained(this), std::move(inner), index));
}

void HlsPlaylistDemuxer::OnInnerSegmentInitialized(
    std::unique_ptr<FFmpegDemuxer> inner, size_t index, Status status) {
  if (stopped_.load()) {
    return;
  }
  if (!status.has_value()) {
    media_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(&HlsPlaylistDemuxer::OnSegmentOpened,
                       base::Unretained(this), index,
                       std::move(open_completion_),
                       base::unexpected(MediaError::Of(
                           ErrorCode::kSourceUnsupported,
                           "the hls segment could not be demuxed",
                           "index = " + std::to_string(index) + ", " +
                               status.error().summary(),
                           "check the segment container against the playlist's "
                           "codecs"))));
    open_completion_ = OpenCompletion();
    return;
  }
  CloseInner();
  inner_ = std::move(inner);
  inner_streams_.clear();
  for (int t = 1; t < static_cast<int>(DemuxerStreamType::kMaxValue); ++t) {
    DemuxerStreamType type = static_cast<DemuxerStreamType>(t);
    if (DemuxerStream* stream = inner_->GetStream(type)) {
      inner_streams_[type] = stream;
    }
  }
  segment_delta_set_ = false;
  cursor_ = index;
  media_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(&HlsPlaylistDemuxer::OnSegmentOpened,
                                base::Unretained(this), index,
                                std::move(open_completion_), Status()));
  open_completion_ = OpenCompletion();
}

void HlsPlaylistDemuxer::OnSegmentOpened(size_t index,
                                         OpenCompletion completion,
                                         Status status) {
  open_in_flight_ = false;
  if (stopped_.load()) {
    return;
  }
  const bool natural_end = !status.has_value() &&
                           status.error().code() == ErrorCode::kSourceEos &&
                           index >= playlist_.segments.size();
  if (!status.has_value() && !natural_end) {
    // A real failure: fail every queued read and the completion itself.
    auto pending = std::move(pending_reads_);
    pending_reads_.clear();
    for (auto& pending_read : pending) {
      std::move(pending_read.read_cb).Run(DemuxerStream::Status::kError, {});
    }
    if (completion.kind == OpenCompletion::Kind::kInit) {
      std::move(completion.init_cb).Run(status);
    } else if (completion.kind == OpenCompletion::Kind::kSeek) {
      std::move(completion.seek_cb).Run(status, base::TimeDelta());
    }
    return;
  }
  if (natural_end) {
    // Serve every queued read the EOS marker; that is the whole point.
    auto pending = std::move(pending_reads_);
    pending_reads_.clear();
    for (auto& pending_read : pending) {
      DemuxerStream::DecoderBufferVector eos;
      eos.push_back(DecoderBuffer::CreateEOSBuffer());
      std::move(pending_read.read_cb)
          .Run(DemuxerStream::Status::kOk, std::move(eos));
    }
    if (completion.kind == OpenCompletion::Kind::kInit) {
      std::move(completion.init_cb)
          .Run(base::unexpected(MediaError::Of(
              ErrorCode::kSourceUnsupported,
              "the hls playlist opened with no playable first segment",
              "url = " + playlist_url_,
              "check that the playlist's segments exist and decode")));
    }
    return;
  }
  // The segment is open: re-issue everything the boundary queued.
  auto pending = std::move(pending_reads_);
  pending_reads_.clear();
  for (auto& pending_read : pending) {
    ReadFromStream(pending_read.stream, pending_read.count,
                   std::move(pending_read.read_cb));
  }
  if (completion.kind == OpenCompletion::Kind::kInit) {
    OnFirstSegmentReady(Status(), std::move(completion.init_cb));
  } else if (completion.kind == OpenCompletion::Kind::kSeek) {
    SeekSegmentOpened(completion.seek_request, std::move(completion.seek_cb));
  }
}

}  // namespace avbase::media

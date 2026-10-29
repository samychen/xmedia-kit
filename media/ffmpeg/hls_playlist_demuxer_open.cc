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
#include "media/ffmpeg/hls_fetch_utils.h"

namespace avbase::media {
namespace {}  // namespace

void HlsPlaylistDemuxer::OpenSegment(size_t index, OpenCompletion completion) {
  open_in_flight_ = true;
  segment_delta_ = base::TimeDelta();
  segment_delta_set_ = false;
  switch_decided_ = false;  // one ABR decision per segment
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
  if (!hls_fetch::FetchWholeUrl(segment.uri, &active_fetch_, &lock_, &body)) {
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
  std::vector<uint8_t> bytes = hls_fetch::StringBytes(body);
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
    // Fetch (or reuse) the 16-byte key, then decrypt in place. Key source:
    // an injected provider wins (its GetKeyBlocking is not interruptible by
    // this demuxer's Stop -- the documented provider contract); without one
    // the built-in clear-key fetch runs, which IS abortable through
    // active_fetch_.
    if (segment.key.uri != cached_key_url_ || cached_key_.empty()) {
      auto provided =
          options_.key_provider
              ? options_.key_provider->GetKeyBlocking(segment.key.uri)
              : hls_fetch::FetchClearKeyBytes(segment.key.uri, &active_fetch_,
                                              &lock_);
      if (!provided.has_value()) {
        media_task_runner_->PostTask(
            FROM_HERE, base::BindOnce(&HlsPlaylistDemuxer::OnSegmentBytesReady,
                                      base::Unretained(this), index,
                                      base::unexpected(provided.error())));
        return;
      }
      if (provided->size() != 16) {
        media_task_runner_->PostTask(
            FROM_HERE,
            base::BindOnce(
                &HlsPlaylistDemuxer::OnSegmentBytesReady,
                base::Unretained(this), index,
                base::unexpected(MediaError::Of(
                    ErrorCode::kSourceUnsupported,
                    "the aes-128 key is not 16 bytes",
                    "url = " + segment.key.uri +
                        ", bytes = " + std::to_string(provided->size()),
                    "check the EXT-X-KEY uri, or inject a key provider"))));
        return;
      }
      cached_key_ = std::move(*provided);
      cached_key_url_ = segment.key.uri;
    }
    std::string iv = hls_fetch::IvFromHex(segment.key.iv_hex);
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
  // ABR sample: the whole-file fetch of this segment over its playback
  // duration is the throughput observation the estimator consumes.
  if (options_.abr_enabled && index < playlist_.segments.size()) {
    abr_estimator_.OnSample(open_segment_bytes_.size(),
                            playlist_.segments[index].duration);
  }
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

// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The read/seek orchestration half of HlsPlaylistDemuxer. The split from
// hls_playlist_demuxer.cc is the C1 line-limit one: that TU owns "how the
// demuxer is opened and what it answers about itself", this one owns "how a
// read crosses a segment boundary". Everything here runs on the media
// sequence except the worker halves (OpenSegmentOnWorker), per the class
// comment in the header.

#include "media/ffmpeg/hls_playlist_demuxer.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "media/ffmpeg/ffmpeg_demuxer.h"

namespace avbase::media {

// ---- the read path
// ------------------------------------------------------------

void HlsPlaylistDemuxer::ReadFromStream(SegmentStream* stream, uint32_t count,
                                        DemuxerStream::ReadCB read_cb) {
  if (stopped_.load()) {
    std::move(read_cb).Run(DemuxerStream::Status::kAborted, {});
    return;
  }
  if (open_in_flight_) {
    pending_reads_.push_back({stream, count, std::move(read_cb)});
    return;
  }
  auto inner_it = inner_streams_.find(stream->type());
  if (inner_it == inner_streams_.end() || inner_ == nullptr) {
    // The segment is drained: advance to the next one with this read
    // queued; OnSegmentOpened re-issues it (or answers EOS).
    pending_reads_.push_back({stream, count, std::move(read_cb)});
    OpenSegment(cursor_ + 1, OpenCompletion());
    return;
  }
  inner_it->second->Read(count,
                         base::BindOnce(&HlsPlaylistDemuxer::OnStreamRead,
                                        base::Unretained(this), stream, count,
                                        std::move(read_cb)));
}

void HlsPlaylistDemuxer::OnStreamRead(
    SegmentStream* stream, uint32_t count, DemuxerStream::ReadCB read_cb,
    DemuxerStream::Status status, DemuxerStream::DecoderBufferVector buffers) {
  if (stopped_.load()) {
    std::move(read_cb).Run(DemuxerStream::Status::kAborted, {});
    return;
  }
  if (status != DemuxerStream::Status::kOk) {
    std::move(read_cb).Run(status, std::move(buffers));
    return;
  }
  const bool only_eos = !buffers.empty() && buffers.back()->IsEndOfStream();
  if (only_eos) {
    // Move to the next segment transparently; the pipeline never sees the
    // seam. Live streams reload the playlist first.
    CloseInner();
    if (playlist_.is_live && cursor_ + 1 >= playlist_.segments.size()) {
      OpenCompletion completion;  // kNone: the read re-issues from pending
      ReloadPlaylist(std::move(completion));
      return;
    }
    pending_reads_.push_back({stream, count, std::move(read_cb)});
    OpenSegment(cursor_ + 1, OpenCompletion());
    return;
  }
  for (auto& buffer : buffers) {
    if (buffer->has_valid_timestamp()) {
      if (!segment_delta_set_) {
        // One delta per segment, taken from the first timestamped buffer:
        // A/V share the container clock, so both streams stay aligned.
        segment_delta_ =
            playlist_.segments[cursor_].timeline_start - buffer->timestamp();
        segment_delta_set_ = true;
      }
      buffer->set_timestamp(buffer->timestamp() + segment_delta_);
      buffer->set_decode_timestamp(buffer->decode_timestamp() + segment_delta_);
    }
    buffer->set_serial(serial_);
    stats_.packets_demuxed++;
  }
  std::move(read_cb).Run(status, std::move(buffers));
}

// ---- live reload
// ---------------------------------------------------------------

void HlsPlaylistDemuxer::ReloadPlaylist(OpenCompletion completion) {
  open_in_flight_ = true;
  worker_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&HlsPlaylistDemuxer::FetchUrlOnWorker,
                     base::Unretained(this), playlist_url_,
                     base::BindOnce(&HlsPlaylistDemuxer::OnPlaylistReloaded,
                                    base::Unretained(this),
                                    std::move(completion))));
}

void HlsPlaylistDemuxer::OnPlaylistReloaded(OpenCompletion completion, bool ok,
                                            std::string body) {
  if (stopped_.load()) {
    return;
  }
  if (ok) {
    auto parsed = hls::ParseM3U8(body, playlist_url_);
    if (parsed.has_value() && !parsed->segments.empty()) {
      // Append only what the served playlist did not have yet, continuing
      // the local timeline from the last known segment.
      const int64_t last_sequence =
          playlist_.segments.empty() ? -1
                                     : playlist_.segments.back().media_sequence;
      base::TimeDelta timeline =
          playlist_.segments.empty()
              ? base::TimeDelta()
              : playlist_.segments.back().timeline_start +
                    playlist_.segments.back().duration;
      for (auto& segment : parsed->segments) {
        if (segment.media_sequence > last_sequence) {
          segment.timeline_start = timeline;
          timeline += segment.duration;
          playlist_.segments.push_back(segment);
        }
      }
      if (!playlist_.segments.empty()) {
        playlist_.total_duration = timeline;
      }
    }
  }
  // Whether or not the reload brought anything: the next open either finds
  // new segments or produces the natural end for the queued reads.
  OpenSegment(cursor_ + 1, std::move(completion));
}

// ---- seek / flush / stop
// --------------------------------------------------------

void HlsPlaylistDemuxer::StartPlayingFrom(base::TimeDelta time, SeekCB cb) {
  if (stopped_.load()) {
    std::move(cb).Run(base::unexpected(MediaError::Of(
                          ErrorCode::kAborted, "the demuxer was stopped",
                          "seek ignored after stop",
                          "this follows Stop(); no action needed")),
                      base::TimeDelta());
    return;
  }
  serial_++;
  const size_t index = SegmentIndexForTime(time);
  if (inner_ != nullptr && index == cursor_) {
    // In-segment seek: hand the inner demuxer the local time.
    const base::TimeDelta local =
        time - playlist_.segments[index].timeline_start;
    inner_->StartPlayingFrom(
        local,
        base::BindOnce(
            [](HlsPlaylistDemuxer* self, base::TimeDelta segment_start,
               SeekCB cb, Status status, base::TimeDelta actual) {
              self->stats_.seek_count++;
              std::move(cb).Run(std::move(status), segment_start + actual);
            },
            base::Unretained(this), playlist_.segments[index].timeline_start,
            std::move(cb)));
    return;
  }
  CloseInner();
  OpenCompletion completion;
  completion.kind = OpenCompletion::Kind::kSeek;
  completion.seek_cb = std::move(cb);
  completion.seek_request = time;
  OpenSegment(index, std::move(completion));
}

void HlsPlaylistDemuxer::SeekSegmentOpened(base::TimeDelta request, SeekCB cb) {
  if (stopped_.load()) {
    std::move(cb).Run(base::unexpected(MediaError::Of(
                          ErrorCode::kAborted, "the demuxer was stopped",
                          "seek ignored after stop", "this follows Stop()")),
                      base::TimeDelta());
    return;
  }
  const base::TimeDelta local =
      request - playlist_.segments[cursor_].timeline_start;
  inner_->StartPlayingFrom(
      local, base::BindOnce(
                 [](HlsPlaylistDemuxer* self, base::TimeDelta segment_start,
                    SeekCB cb, Status status, base::TimeDelta actual) {
                   self->stats_.seek_count++;
                   std::move(cb).Run(std::move(status), segment_start + actual);
                 },
                 base::Unretained(this),
                 playlist_.segments[cursor_].timeline_start, std::move(cb)));
}

void HlsPlaylistDemuxer::Flush(base::OnceClosure flush_cb) {
  serial_++;
  if (inner_ != nullptr) {
    inner_->Flush(std::move(flush_cb));
    return;
  }
  std::move(flush_cb).Run();
}

void HlsPlaylistDemuxer::Reset(base::OnceClosure reset_cb) {
  if (inner_ != nullptr) {
    inner_->Reset(std::move(reset_cb));
    return;
  }
  std::move(reset_cb).Run();
}

void HlsPlaylistDemuxer::Stop() {
  if (stopped_.exchange(true)) {
    return;
  }
  {
    std::lock_guard<std::mutex> scoped(lock_);
    if (active_fetch_ != nullptr) {
      active_fetch_->Abort();
    }
  }
  if (worker_.IsRunning()) {
    worker_.Stop();
  }
  inner_.reset();
  inner_streams_.clear();
  auto pending = std::move(pending_reads_);
  pending_reads_.clear();
  for (auto& pending_read : pending) {
    std::move(pending_read.read_cb).Run(DemuxerStream::Status::kAborted, {});
  }
}

}  // namespace avbase::media

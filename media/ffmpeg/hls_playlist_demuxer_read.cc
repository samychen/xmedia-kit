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

void HlsPlaylistDemuxer::AdvanceAfterBoundary(OpenCompletion completion) {
  // The boundary executor: run a stored ABR switch first (fetch the target
  // rung's playlist), then open the next segment of whatever playlist is
  // active.
  if (pending_switch_rung_ >= 0 && pending_switch_rung_ != active_rung_) {
    const int variant_index = abr_ladder_[pending_switch_rung_].variant_index;
    pending_switch_rung_ = -1;
    StartVariantSwitch(variant_index, std::move(completion));
    return;
  }
  pending_switch_rung_ = -1;
  OpenSegment(cursor_ + 1, std::move(completion));
}

void HlsPlaylistDemuxer::MaybeDecideSwitch() {
  if (!options_.abr_enabled || abr_ladder_.empty() || switch_decided_ ||
      inner_ == nullptr) {
    return;
  }
  // One decision per segment, taken while the segment is still playing (so
  // the buffer signal is real); executed at the boundary.
  base::TimeDelta buffered;
  for (const auto& [type, stream] : inner_streams_) {
    buffered = std::max(buffered, stream->buffered_duration());
  }
  AbrInput input;
  input.estimated_bandwidth_bps = abr_estimator_.bits_per_second();
  input.variant_bandwidths_bps.reserve(abr_ladder_.size());
  for (const auto& variant : abr_ladder_) {
    input.variant_bandwidths_bps.push_back(variant.bandwidth);
  }
  input.current_index = std::max(active_rung_, 0);
  input.current_variant_bandwidth_bps =
      abr_ladder_[input.current_index].bandwidth;
  // The segment itself is the buffer capacity proxy: the inner demuxer
  // queues at most one segment ahead. Decide only once the queue holds at
  // least half the segment -- the first read after a boundary sees a nearly
  // empty queue (the inner demuxer is still filling), and a decision taken
  // there would always panic to the floor and never climb.
  if (cursor_ >= playlist_.segments.size()) {
    return;
  }
  input.buffer_capacity = playlist_.segments[cursor_].duration;
  input.buffered_duration = buffered;
  if (input.buffer_capacity <= base::TimeDelta() ||
      buffered.InMicroseconds() * 2 < input.buffer_capacity.InMicroseconds()) {
    return;  // not enough signal yet; decide on a later read
  }
  const AbrDecision decision = abr_algo_->Decide(input);
  switch_decided_ = true;
  if (decision.target_index != active_rung_ && decision.target_index >= 0 &&
      decision.target_index < static_cast<int>(abr_ladder_.size())) {
    pending_switch_rung_ = decision.target_index;
  }
}

void HlsPlaylistDemuxer::StartVariantSwitch(int variant_index,
                                            OpenCompletion completion) {
  // Fetch the target rung's media playlist; OnVariantSwitchLoaded replaces
  // the active segments and opens the aligned segment index. The ladder is
  // assumed aligned across rungs (same segment count and durations), which
  // is what every real ladder ships -- CicadaPlayer aligns by sequence
  // number to the same effect.
  open_in_flight_ = true;
  active_variant_pending_ = variant_index;
  worker_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(
          &HlsPlaylistDemuxer::FetchUrlOnWorker, base::Unretained(this),
          abr_ladder_.empty()
              ? std::string()
              : master_variants_[static_cast<size_t>(variant_index)].uri,
          base::BindOnce(&HlsPlaylistDemuxer::OnVariantSwitchLoaded,
                         base::Unretained(this), variant_index)));
}

void HlsPlaylistDemuxer::OnVariantSwitchLoaded(int variant_index, bool ok,
                                               std::string body) {
  if (stopped_.load()) {
    return;
  }
  if (!ok) {
    // Stay on the current rung rather than failing playback for an ABR
    // upgrade; the next boundary tries again.
    open_in_flight_ = false;
    auto pending = std::move(pending_reads_);
    pending_reads_.clear();
    for (auto& pending_read : pending) {
      ReadFromStream(pending_read.stream, pending_read.count,
                     std::move(pending_read.read_cb));
    }
    return;
  }
  auto parsed = hls::ParseM3U8(body, master_variants_[variant_index].uri);
  if (parsed.has_value() && !parsed->segments.empty()) {
    playlist_ = std::move(*parsed);
    active_variant_ = variant_index;
    active_rung_ = RungForVariant(variant_index);
    stats_.variant_switches++;
  }
  open_in_flight_ = false;
  OpenSegment(cursor_ + 1, OpenCompletion());
}

int HlsPlaylistDemuxer::RungForVariant(int variant_index) const {
  for (int i = 0; i < static_cast<int>(abr_ladder_.size()); ++i) {
    if (abr_ladder_[i].variant_index == variant_index) {
      return i;
    }
  }
  return 0;
}

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
    AdvanceAfterBoundary(OpenCompletion());
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
    AdvanceAfterBoundary(OpenCompletion());
    return;
  }
  MaybeDecideSwitch();
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

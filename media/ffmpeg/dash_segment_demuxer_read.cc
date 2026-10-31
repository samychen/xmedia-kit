// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The read/seek/open-orchestration half of DashSegmentDemuxer, split from
// dash_segment_demuxer.cc for the C1 line limit (same seam as the HLS
// demuxer's). This TU owns "how a read crosses a segment boundary" and the
// segment-open chain it feeds: fetch (init prepend) -> MemoryDataSource ->
// inner FFmpegDemuxer -> timestamp rebase. Everything here runs on the media
// sequence except the Fetch*OnWorker halves.

#include "media/ffmpeg/dash_segment_demuxer.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "media/ffmpeg/ffmpeg_demuxer.h"
#include "media/ffmpeg/hls_fetch_utils.h"

namespace xmedia::media {

// ---- the open chain
// ----------------------------------------------------------

// ---- the read path
// ------------------------------------------------------------

void DashSegmentDemuxer::ReadFromStream(DemuxerStreamType type, uint32_t count,
                                        DemuxerStream::ReadCB read_cb) {
  if (stopped_.load()) {
    std::move(read_cb).Run(DemuxerStream::Status::kAborted, {});
    return;
  }
  Chain* chain = chain_for_type(type);
  if (chain == nullptr) {
    std::move(read_cb).Run(DemuxerStream::Status::kAborted, {});
    return;
  }
  if (chain->open_in_flight) {
    pending_reads_.push_back({type, count, std::move(read_cb)});
    return;
  }
  auto inner_it = chain->inner_streams.find(type);
  if (chain->inner == nullptr || inner_it == chain->inner_streams.end()) {
    // The chain is drained: advance to the next segment with this read
    // queued; the settle re-issues it (or answers EOS).
    pending_reads_.push_back({type, count, std::move(read_cb)});
    AdvanceChain(chain_index_for_type(type));
    return;
  }
  inner_it->second->Read(
      count,
      base::BindOnce(&DashSegmentDemuxer::OnStreamRead, base::Unretained(this),
                     type, count, std::move(read_cb)));
}

void DashSegmentDemuxer::OnStreamRead(
    DemuxerStreamType type, uint32_t count, DemuxerStream::ReadCB read_cb,
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
    // Move to the next segment of this chain transparently; the pipeline
    // never sees the seam. List exhausted: the chain's natural EOS goes out.
    const size_t chain_index = chain_index_for_type(type);
    Chain& chain = chains_[chain_index];
    CloseChainInner(chain_index);
    if (chain.cursor + 1 >= chain.media_urls.size()) {
      chain.ended = true;
      std::move(read_cb).Run(status, std::move(buffers));
      return;
    }
    pending_reads_.push_back({type, count, std::move(read_cb)});
    AdvanceChain(chain_index);
    return;
  }
  Chain& chain = chains_[chain_index_for_type(type)];
  for (auto& buffer : buffers) {
    if (buffer->has_valid_timestamp()) {
      if (!chain.segment_delta_set) {
        // One delta per segment, taken from the first timestamped buffer:
        // the chain's inner segments each start near their own zero (the
        // init segment was prepended), and the chain's timeline position is
        // the uniform SegmentTemplate grid.
        chain.segment_delta = chain.TimelineFor(chain.cursor) -
                              buffer->timestamp();
        chain.segment_delta_set = true;
      }
      buffer->set_timestamp(buffer->timestamp() + chain.segment_delta);
      buffer->set_decode_timestamp(buffer->decode_timestamp() +
                                   chain.segment_delta);
    }
    buffer->set_serial(serial_);
    stats_.packets_demuxed++;
  }
  std::move(read_cb).Run(status, std::move(buffers));
}

void DashSegmentDemuxer::AdvanceChain(size_t chain_index) {
  Chain& chain = chains_[chain_index];
  if (chain.open_in_flight) {
    return;  // the settle will re-issue the queued reads
  }
  OpenSegment(chain_index, chain.cursor + 1);
}

void DashSegmentDemuxer::CloseChainInner(size_t chain_index) {
  // Only the EOS / seek paths call this: both run after the inner stream
  // answered its last read, so there is no outstanding inner work.
  Chain& chain = chains_[chain_index];
  chain.inner.reset();
  chain.inner_streams.clear();
}

// ---- seek / flush / stop
// --------------------------------------------------------

size_t DashSegmentDemuxer::SegmentIndexForTime(size_t chain_index,
                                               base::TimeDelta time) const {
  const Chain& chain = chains_[chain_index];
  if (chain.media_duration <= base::TimeDelta()) {
    return 0;
  }
  const int64_t index = time / chain.media_duration;
  return static_cast<size_t>(
      std::clamp<int64_t>(index, 0,
                          static_cast<int64_t>(chain.media_urls.size()) - 1));
}

void DashSegmentDemuxer::StartPlayingFrom(base::TimeDelta time, SeekCB cb) {
  if (stopped_.load()) {
    std::move(cb).Run(base::unexpected(MediaError::Of(
                          ErrorCode::kAborted, "the demuxer was stopped",
                          "seek ignored after stop",
                          "this follows Stop(); no action needed")),
                      base::TimeDelta());
    return;
  }
  serial_++;
  // All chains seek to the same segment grid index (uniform SegmentTemplate
  // durations make the grid shared); the settle runs each chain's inner
  // StartPlayingFrom and the seek answers when the LAST one lands.
  phase_ = Phase::kSeek;
  seek_request_ = time;
  seek_cb_ = std::move(cb);
  seek_chains_pending_ = static_cast<int>(chains_.size());
  for (size_t i = 0; i < chains_.size(); ++i) {
    const size_t index = SegmentIndexForTime(i, time);
    Chain& chain = chains_[i];
    if (chain.inner != nullptr && chain.cursor == index) {
      RunSeekOnChain(i);  // in-segment: no reopen needed
      continue;
    }
    CloseChainInner(i);
    OpenSegment(i, index);
  }
}

void DashSegmentDemuxer::RunSeekOnChain(size_t chain_index) {
  Chain& chain = chains_[chain_index];
  const base::TimeDelta local =
      seek_request_ - chain.TimelineFor(chain.cursor);
  if (chain.inner == nullptr) {
    // The chain had nothing open (past-EOF before the seek): reopening at
    // the target was the whole seek for it; the settle already ran.
    OnChainSeekDone(chain_index, Status(), chain.TimelineFor(chain.cursor));
    return;
  }
  chain.inner->StartPlayingFrom(
      std::max(local, base::TimeDelta()),
      base::BindOnce(
          [](DashSegmentDemuxer* self, size_t chain_index,
             base::TimeDelta segment_start, Status status,
             base::TimeDelta actual) {
            self->stats_.seek_count++;
            self->OnChainSeekDone(chain_index, status,
                                  segment_start + actual);
          },
          base::Unretained(this), chain_index,
          chain.TimelineFor(chain.cursor)));
}

void DashSegmentDemuxer::OnChainSeekDone(size_t chain_index, Status status,
                                         base::TimeDelta actual) {
  if (stopped_.load()) {
    return;
  }
  if (!status.has_value()) {
    seek_chains_pending_ = 0;
    if (seek_cb_) {
      std::move(seek_cb_).Run(status, base::TimeDelta());
    }
    phase_ = Phase::kPlaying;
    return;
  }
  // Keep the best actual (the video chain's answer is the timeline's).
  if (!seek_actual_.has_value() || actual > *seek_actual_) {
    seek_actual_ = actual;
  }
  if (--seek_chains_pending_ > 0) {
    return;
  }
  phase_ = Phase::kPlaying;
  if (seek_cb_) {
    std::move(seek_cb_).Run(Status(), seek_actual_.value_or(seek_request_));
  }
  seek_actual_ = std::nullopt;
}

void DashSegmentDemuxer::Flush(base::OnceClosure flush_cb) {
  serial_++;
  // Chains flush in order; the callback runs after the last one (a flushed
  // inner answers synchronously through its own sequence, so a counter and
  // one hop each is the same shape as the HLS single-chain flush).
  auto remaining = std::make_shared<int>(0);
  for (auto& chain : chains_) {
    if (chain.inner != nullptr) {
      (*remaining)++;
    }
  }
  if (*remaining == 0) {
    std::move(flush_cb).Run();
    return;
  }
  for (auto& chain : chains_) {
    if (chain.inner == nullptr) {
      continue;
    }
    chain.inner->Flush(base::BindOnce(
        [](std::shared_ptr<int> remaining, base::OnceClosure flush_cb) {
          if (--*remaining == 0) {
            std::move(flush_cb).Run();
          }
        },
        remaining, std::move(flush_cb)));
  }
}

void DashSegmentDemuxer::Reset(base::OnceClosure reset_cb) {
  // Chains reset in order; the callback runs after the last one.
  auto remaining = std::make_shared<int>(0);
  for (auto& chain : chains_) {
    if (chain.inner != nullptr) {
      (*remaining)++;
    }
  }
  if (*remaining == 0) {
    std::move(reset_cb).Run();
    return;
  }
  for (auto& chain : chains_) {
    if (chain.inner == nullptr) {
      continue;
    }
    chain.inner->Reset(base::BindOnce(
        [](std::shared_ptr<int> remaining, base::OnceClosure reset_cb) {
          if (--*remaining == 0) {
            std::move(reset_cb).Run();
          }
        },
        remaining, std::move(reset_cb)));
  }
}

void DashSegmentDemuxer::Stop() {
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
  for (auto& chain : chains_) {
    chain.inner.reset();
    chain.inner_streams.clear();
  }
  auto pending = std::move(pending_reads_);
  pending_reads_.clear();
  for (auto& pending_read : pending) {
    std::move(pending_read.read_cb).Run(DemuxerStream::Status::kAborted, {});
  }
}

}  // namespace xmedia::media

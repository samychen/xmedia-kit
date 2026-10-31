// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The segment-open chain of DashSegmentDemuxer, split from the read
// orchestration TU (dash_segment_demuxer_read.cc) for the C1 line limit --
// the same seam as the HLS demuxer's. This TU owns "turn SegmentTemplate
// segment N into an open inner FFmpegDemuxer": fetch (init prepend),
// MemoryDataSource, probe -- plus the per-phase settle gate that the read
// TU's boundaries fall into. Everything here runs on the media sequence
// except FetchSegmentOnWorker.

#include "media/ffmpeg/dash_segment_demuxer.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "media/ffmpeg/ffmpeg_demuxer.h"
#include "media/ffmpeg/hls_fetch_utils.h"

namespace xmedia::media {

void DashSegmentDemuxer::OpenSegment(size_t chain_index, size_t segment_index) {
  Chain& chain = chains_[chain_index];
  chain.open_in_flight = true;
  chain.segment_delta = base::TimeDelta();
  chain.segment_delta_set = false;
  if (segment_index >= chain.media_urls.size()) {
    // Past the end: the natural-EOF shape for this chain. Nothing is
    // fetched; OnSegmentOpened's settle path serves the queued reads EOS.
    chain.cursor = segment_index;
    media_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(&DashSegmentDemuxer::OnSegmentOpened,
                                  base::Unretained(this), chain_index,
                                  segment_index,
                                  base::unexpected(MediaError::Of(
                                      ErrorCode::kSourceEos, "no more segments",
                                      "chain = " + std::to_string(chain_index) +
                                          ", index = " +
                                          std::to_string(segment_index),
                                      "this is the natural end of the "
                                      "representation"))));
    return;
  }
  chain.cursor = segment_index;
  worker_.task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&DashSegmentDemuxer::FetchSegmentOnWorker,
                                base::Unretained(this), chain_index,
                                segment_index,
                                chain.media_urls[segment_index]));
}

void DashSegmentDemuxer::FetchSegmentOnWorker(size_t chain_index,
                                              size_t segment_index,
                                              std::string url) {
  Chain& chain = chains_[chain_index];
  // The initialisation segment is fetched once per chain, lazily, on the
  // first media-segment fetch: one worker round-trip covers both, which
  // keeps the open state machine single-phase.
  if (!chain.init_pending_url.empty()) {
    std::string init_body;
    if (!hls_fetch::FetchWholeUrl(chain.init_pending_url, &active_fetch_,
                                  &lock_, &init_body)) {
      media_task_runner_->PostTask(
          FROM_HERE,
          base::BindOnce(
              &DashSegmentDemuxer::OnSegmentBytesReady, base::Unretained(this),
              chain_index, segment_index,
              base::unexpected(MediaError::Of(
                  ErrorCode::kSourceReadFailed,
                  "the dash initialisation segment could not be fetched",
                  "url = " + chain.init_pending_url,
                  "check the SegmentTemplate's initialization attribute"))));
      return;
    }
    chain.init_bytes = hls_fetch::StringBytes(init_body);
    chain.init_pending_url.clear();
  }
  std::string body;
  if (!hls_fetch::FetchWholeUrl(url, &active_fetch_, &lock_, &body)) {
    media_task_runner_->PostTask(
        FROM_HERE,
        base::BindOnce(
            &DashSegmentDemuxer::OnSegmentBytesReady, base::Unretained(this),
            chain_index, segment_index,
            base::unexpected(MediaError::Of(
                ErrorCode::kSourceReadFailed,
                "the dash segment could not be fetched", "url = " + url,
                "the retry decorator upstream reconnects on transport "
                "errors; this error means the segment is really gone"))));
    return;
  }
  std::vector<uint8_t> bytes = hls_fetch::StringBytes(body);
  if (!chain.init_bytes.empty()) {
    // fMP4: the initialisation segment must precede every media segment for
    // FFmpeg to see a parseable file. Concatenation is the whole reason the
    // bytes are cached per chain.
    bytes.insert(bytes.begin(), chain.init_bytes.begin(),
                 chain.init_bytes.end());
  }
  media_task_runner_->PostTask(
      FROM_HERE,
      base::BindOnce(
          &DashSegmentDemuxer::OnSegmentBytesReady, base::Unretained(this),
          chain_index, segment_index,
          base::expected<std::vector<uint8_t>, MediaError>(std::move(bytes))));
}

void DashSegmentDemuxer::OnSegmentBytesReady(
    size_t chain_index, size_t segment_index,
    base::expected<std::vector<uint8_t>, MediaError> bytes_or_error) {
  if (stopped_.load()) {
    return;
  }
  if (!bytes_or_error.has_value()) {
    OnSegmentOpened(chain_index, segment_index,
                    base::unexpected(bytes_or_error.error()));
    return;
  }
  auto inner = std::make_unique<FFmpegDemuxer>(media_log_, limits_);
  auto memory =
      base::MakeRefCounted<MemoryDataSource>(bytes_or_error->data(),
                                             bytes_or_error->size());
  DemuxerOptions inner_options = options_;
  inner_options.reconnect = false;  // the segment is already fully in memory
  inner->Initialize(
      DataSourceDescriptor::FromSource(std::move(memory)), inner_options, host_,
      media_task_runner_,
      base::BindOnce(&DashSegmentDemuxer::OnInnerSegmentInitialized,
                     base::Unretained(this), std::move(inner), chain_index,
                     segment_index));
}

void DashSegmentDemuxer::OnInnerSegmentInitialized(
    std::unique_ptr<FFmpegDemuxer> inner, size_t chain_index,
    size_t segment_index, Status status) {
  if (stopped_.load()) {
    return;
  }
  if (!status.has_value()) {
    OnSegmentOpened(chain_index, segment_index,
                    base::unexpected(MediaError::Of(
                        ErrorCode::kSourceUnsupported,
                        "the dash segment could not be demuxed",
                        "chain = " + std::to_string(chain_index) +
                            ", index = " + std::to_string(segment_index) +
                            ", " + status.error().summary(),
                        "check the segment container against the mpd's "
                        "codecs")));
    return;
  }
  Chain& chain = chains_[chain_index];
  CloseChainInner(chain_index);
  chain.inner = std::move(inner);
  chain.inner_streams.clear();
  for (int t = 1; t < static_cast<int>(DemuxerStreamType::kMaxValue); ++t) {
    DemuxerStreamType type = static_cast<DemuxerStreamType>(t);
    if (DemuxerStream* stream = chain.inner->GetStream(type)) {
      chain.inner_streams[type] = stream;
    }
  }
  chain.segment_delta_set = false;
  chain.cursor = segment_index;
  // First open anywhere: claim every type the inner provides (a muxed TS
  // rep covers video AND audio from one chain).
  for (const auto& [type, inner_stream] : chain.inner_streams) {
    if (type_owner_.find(type) == type_owner_.end()) {
      type_owner_[type] = chain_index;
      streams_[type] = std::make_unique<SegmentStream>(this, type);
    }
  }
  OnSegmentOpened(chain_index, segment_index, Status());
}

void DashSegmentDemuxer::OnSegmentOpened(size_t chain_index,
                                         size_t segment_index, Status status) {
  Chain& chain = chains_[chain_index];
  chain.open_in_flight = false;
  if (stopped_.load()) {
    return;
  }
  if (!status.has_value() && status.error().code() != ErrorCode::kSourceEos) {
    // A real failure: fail every queued read; the phase gate decides what
    // the open/seek itself reports.
    auto pending = std::move(pending_reads_);
    pending_reads_.clear();
    for (auto& pending_read : pending) {
      std::move(pending_read.read_cb).Run(DemuxerStream::Status::kError, {});
    }
  }
  OnChainSettled(chain_index, std::move(status));
}

void DashSegmentDemuxer::OnChainSettled(size_t chain_index, Status status) {
  switch (phase_) {
    case Phase::kInit:
      // Every chain reports exactly once (EOS on segment 0 counts as a
      // failed init for that chain -- there is nothing to play there).
      FinishInit(std::move(status));
      return;
    case Phase::kSeek:
      if (!status.has_value()) {
        // A failed chain open fails the seek; answer with the actual time
        // unknown (the pipeline treats a failed seek as terminal for the
        // request, playback keeps whatever was playing).
        seek_chains_pending_ = 0;
        if (seek_cb_) {
          std::move(seek_cb_).Run(status, base::TimeDelta());
        }
        phase_ = Phase::kPlaying;
        return;
      }
      RunSeekOnChain(chain_index);
      return;
    case Phase::kPlaying:
      // Re-issue everything this chain's boundary queued.
      for (auto it = pending_reads_.begin(); it != pending_reads_.end();) {
        if (chain_index_for_type(it->type) == chain_index) {
          auto type = it->type;
          auto count = it->count;
          auto cb = std::move(it->read_cb);
          it = pending_reads_.erase(it);
          ReadFromStream(type, count, std::move(cb));
          continue;
        }
        ++it;
      }
      return;
  }
}

void DashSegmentDemuxer::FinishInit(Status status) {
  // A chain that settled with a real error (not its natural EOS) marks the
  // whole open failed; the answer still waits for the remaining chains so
  // their worker fetches have somewhere to land.
  if (!status.has_value() && status.error().code() != ErrorCode::kSourceEos) {
    init_failed_ = true;
  }
  if (--init_chains_pending_ > 0) {
    return;  // other chains still opening
  }
  const bool any_playable =
      !init_failed_ &&
      std::any_of(chains_.begin(), chains_.end(), [](const Chain& chain) {
        return chain.inner != nullptr && !chain.inner_streams.empty();
      });
  if (!any_playable) {
    init_failed_ = true;
    auto cb = std::move(init_cb_);
    init_cb_ = InitializeCB();
    phase_ = Phase::kPlaying;
    std::move(cb).Run(base::unexpected(MediaError::Of(
        ErrorCode::kSourceUnsupported,
        "the dash mpd opened with no playable first segment",
        "url = " + mpd_url_,
        "check that the segments exist and their container matches the "
        "mpd's codecs")));
    return;
  }
  // The video-type chain (first) owns MediaInfo; duration is the manifest's.
  for (auto& chain : chains_) {
    if (chain.inner != nullptr && !chain.inner_streams.empty()) {
      media_info_ = chain.inner->media_info();
      break;
    }
  }
  media_info_.duration = mpd_.media_presentation_duration;
  media_info_.uri = mpd_url_;
  media_info_.format_name += ";dash";
  initialized_ = true;
  phase_ = Phase::kPlaying;
  auto cb = std::move(init_cb_);
  init_cb_ = InitializeCB();
  std::move(cb).Run(Status());
}

}  // namespace xmedia::media

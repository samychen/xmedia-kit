// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

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

// ---- lifecycle
// ---------------------------------------------------------------

HlsPlaylistDemuxer::HlsPlaylistDemuxer(base::scoped_refptr<MediaLog> media_log,
                                       BufferQueueLimits limits)
    : limits_(limits), media_log_(std::move(media_log)) {}

HlsPlaylistDemuxer::~HlsPlaylistDemuxer() {
  Stop();
}

void HlsPlaylistDemuxer::Initialize(
    const DataSourceDescriptor& source, const DemuxerOptions& options,
    Host* host,
    base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
    InitializeCB init_cb) {
  source_ = source;
  options_ = options;
  host_ = host;
  media_task_runner_ = std::move(media_task_runner);
  playlist_url_ = source_.uri;

  if (!worker_.Start()) {
    media_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(std::move(init_cb),
                                  base::unexpected(MediaError::Of(
                                      ErrorCode::kSourceOpenFailed,
                                      "the hls worker thread failed to start",
                                      "url = " + playlist_url_,
                                      "check system thread limits"))));
    return;
  }
  FetchPlaylist(std::move(init_cb));
}

void HlsPlaylistDemuxer::FetchPlaylist(InitializeCB init_cb) {
  worker_.task_runner()->PostTask(
      FROM_HERE,
      base::BindOnce(&HlsPlaylistDemuxer::FetchUrlOnWorker,
                     base::Unretained(this), playlist_url_,
                     base::BindOnce(&HlsPlaylistDemuxer::OnPlaylistFetched,
                                    base::Unretained(this),
                                    std::move(init_cb))));
}

void HlsPlaylistDemuxer::FetchUrlOnWorker(
    const std::string& url, base::OnceCallback<void(bool, std::string)> done) {
  std::string body;
  const bool ok = hls_fetch::FetchWholeUrl(url, &active_fetch_, &lock_, &body);
  media_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(std::move(done), ok, std::move(body)));
}

void HlsPlaylistDemuxer::OnPlaylistFetched(InitializeCB init_cb, bool ok,
                                           std::string body) {
  if (stopped_.load()) {
    return;
  }
  if (!ok) {
    std::move(init_cb).Run(base::unexpected(MediaError::Of(
        ErrorCode::kSourceOpenFailed, "the playlist could not be fetched",
        "url = " + playlist_url_, "check the url and the server's response")));
    return;
  }
  auto parsed = hls::ParseM3U8(body, playlist_url_);
  if (!parsed.has_value()) {
    std::move(init_cb).Run(base::unexpected(parsed.error()));
    return;
  }
  playlist_ = std::move(*parsed);
  if (playlist_.is_master) {
    // The ABR ladder: the master's variants sorted ascending by bandwidth.
    abr_ladder_.clear();
    for (int i = 0; i < static_cast<int>(playlist_.variants.size()); ++i) {
      abr_ladder_.push_back(playlist_.variants[i]);
      abr_ladder_.back().variant_index = i;
    }
    std::sort(abr_ladder_.begin(), abr_ladder_.end(),
              [](const hls::Variant& a, const hls::Variant& b) {
                return a.bandwidth < b.bandwidth;
              });
    master_variants_ = playlist_.variants;
    // Starting rung: CicadaPlayer's SetDefaultBandWidth semantics -- the
    // highest rung the initial estimate affords; without an estimate the
    // highest advertised (the old rule).
    int start = static_cast<int>(abr_ladder_.size()) - 1;
    if (options_.abr_enabled && options_.abr_initial_bandwidth_bps > 0) {
      start = 0;
      for (int i = 0; i < static_cast<int>(abr_ladder_.size()); ++i) {
        if (abr_ladder_[i].bandwidth <= options_.abr_initial_bandwidth_bps) {
          start = i;
        }
      }
    }
    active_variant_ = abr_ladder_[start].variant_index;
    active_rung_ = start;
    if (options_.abr_enabled) {
      abr_algo_ = std::make_unique<AbrBufferAlgoStrategy>();
    }
    const std::string variant_url = master_variants_[active_variant_].uri;
    worker_.task_runner()->PostTask(
        FROM_HERE,
        base::BindOnce(&HlsPlaylistDemuxer::FetchUrlOnWorker,
                       base::Unretained(this), variant_url,
                       base::BindOnce(&HlsPlaylistDemuxer::OnVariantFetched,
                                      base::Unretained(this),
                                      std::move(init_cb), variant_url)));
    return;
  }
  OpenFirstSegment(std::move(init_cb));
}

void HlsPlaylistDemuxer::OnVariantFetched(InitializeCB init_cb,
                                          const std::string& variant_url,
                                          bool ok, std::string body) {
  if (stopped_.load()) {
    return;
  }
  if (!ok) {
    std::move(init_cb).Run(base::unexpected(MediaError::Of(
        ErrorCode::kSourceOpenFailed,
        "the variant playlist could not be fetched", "url = " + variant_url,
        "check the master playlist's variant urls")));
    return;
  }
  auto parsed = hls::ParseM3U8(body, variant_url);
  if (!parsed.has_value() || parsed->segments.empty()) {
    std::move(init_cb).Run(base::unexpected(
        parsed.has_value()
            ? MediaError::Of(ErrorCode::kSourceUnsupported,
                             "the variant playlist has no segments",
                             "url = " + variant_url,
                             "check the media playlist contents")
            : parsed.error()));
    return;
  }
  playlist_ = std::move(*parsed);
  OpenFirstSegment(std::move(init_cb));
}

void HlsPlaylistDemuxer::OpenFirstSegment(InitializeCB init_cb) {
  open_in_flight_ = true;
  pending_reads_.clear();
  OpenCompletion completion;
  completion.kind = OpenCompletion::Kind::kInit;
  completion.init_cb = std::move(init_cb);
  OpenSegment(0, std::move(completion));
}

void HlsPlaylistDemuxer::OnFirstSegmentReady(Status status,
                                             InitializeCB init_cb) {
  open_in_flight_ = false;
  if (stopped_.load()) {
    return;
  }
  if (!status.has_value()) {
    std::move(init_cb).Run(status);
    return;
  }
  if (inner_streams_.empty()) {
    std::move(init_cb).Run(base::unexpected(MediaError::Of(
        ErrorCode::kSourceUnsupported, "the first hls segment has no streams",
        "url = " + playlist_.segments[cursor_].uri,
        "check that the segment is a valid ts or fmp4 body")));
    return;
  }
  for (const auto& [type, inner_stream] : inner_streams_) {
    streams_[type] = std::make_unique<SegmentStream>(this, type);
  }
  media_info_ = inner_->media_info();
  media_info_.duration = playlist_.total_duration;
  media_info_.uri = playlist_url_;
  media_info_.format_name += ";hls";
  initialized_ = true;
  std::move(init_cb).Run(Status());
}

// ---- queries
// -----------------------------------------------------------------

DemuxerStream* HlsPlaylistDemuxer::GetStream(DemuxerStreamType type) {
  auto it = streams_.find(type);
  return it == streams_.end() ? nullptr : it->second.get();
}

void HlsPlaylistDemuxer::SetActiveStream(DemuxerStreamType type,
                                         int stream_index) {
  if (inner_ != nullptr) {
    inner_->SetActiveStream(type, stream_index);
  }
}

const MediaInfo& HlsPlaylistDemuxer::media_info() const {
  return media_info_;
}

base::TimeDelta HlsPlaylistDemuxer::GetStartTime() const {
  return base::TimeDelta();
}

bool HlsPlaylistDemuxer::IsLive() const {
  return playlist_.is_live;
}

bool HlsPlaylistDemuxer::IsSeekable() const {
  return !playlist_.is_live;
}

DemuxerStats HlsPlaylistDemuxer::GetStats() const {
  return stats_;
}

const char* HlsPlaylistDemuxer::name() const {
  return "HlsPlaylistDemuxer";
}

size_t HlsPlaylistDemuxer::GetBufferedPacketCount() const {
  return inner_ != nullptr ? inner_->GetBufferedPacketCount() : 0;
}

size_t HlsPlaylistDemuxer::SegmentIndexForTime(base::TimeDelta time) const {
  size_t index = 0;
  for (size_t i = 0; i < playlist_.segments.size(); ++i) {
    if (playlist_.segments[i].timeline_start <= time) {
      index = i;
    }
  }
  return index;
}

void HlsPlaylistDemuxer::CloseInner() {
  // Only the EOS / seek paths call this: both run after the inner stream
  // answered its last read, so there is no outstanding inner work.
  inner_.reset();
  inner_streams_.clear();
}

// ---- SegmentStream
// -------------------------------------------------------------

HlsPlaylistDemuxer::SegmentStream::SegmentStream(HlsPlaylistDemuxer* demuxer,
                                                 DemuxerStreamType type)
    : demuxer_(demuxer), type_(type) {}

HlsPlaylistDemuxer::SegmentStream::~SegmentStream() = default;

void HlsPlaylistDemuxer::SegmentStream::Read(uint32_t count, ReadCB read_cb) {
  demuxer_->ReadFromStream(this, count, std::move(read_cb));
}

const AudioDecoderConfig&
HlsPlaylistDemuxer::SegmentStream::audio_decoder_config() const {
  static const AudioDecoderConfig empty;
  auto it = demuxer_->inner_streams_.find(type_);
  return it == demuxer_->inner_streams_.end()
             ? empty
             : it->second->audio_decoder_config();
}

const VideoDecoderConfig&
HlsPlaylistDemuxer::SegmentStream::video_decoder_config() const {
  static const VideoDecoderConfig empty;
  auto it = demuxer_->inner_streams_.find(type_);
  return it == demuxer_->inner_streams_.end()
             ? empty
             : it->second->video_decoder_config();
}

const TextDecoderConfig&
HlsPlaylistDemuxer::SegmentStream::text_decoder_config() const {
  static const TextDecoderConfig empty;
  auto it = demuxer_->inner_streams_.find(type_);
  return it == demuxer_->inner_streams_.end()
             ? empty
             : it->second->text_decoder_config();
}

int32_t HlsPlaylistDemuxer::SegmentStream::stream_index() const {
  auto it = demuxer_->inner_streams_.find(type_);
  return it == demuxer_->inner_streams_.end() ? 0 : it->second->stream_index();
}

StreamLiveness HlsPlaylistDemuxer::SegmentStream::liveness() const {
  return demuxer_->IsLive() ? StreamLiveness::kLive : StreamLiveness::kRecorded;
}

bool HlsPlaylistDemuxer::SegmentStream::SupportsConfigChanges() const {
  auto it = demuxer_->inner_streams_.find(type_);
  return it != demuxer_->inner_streams_.end() &&
         it->second->SupportsConfigChanges();
}

int32_t HlsPlaylistDemuxer::SegmentStream::serial() const {
  return demuxer_->serial_;
}

size_t HlsPlaylistDemuxer::SegmentStream::buffered_buffers() const {
  auto it = demuxer_->inner_streams_.find(type_);
  return it == demuxer_->inner_streams_.end() ? 0
                                              : it->second->buffered_buffers();
}

size_t HlsPlaylistDemuxer::SegmentStream::buffered_bytes() const {
  auto it = demuxer_->inner_streams_.find(type_);
  return it == demuxer_->inner_streams_.end() ? 0
                                              : it->second->buffered_bytes();
}

base::TimeDelta HlsPlaylistDemuxer::SegmentStream::buffered_duration() const {
  auto it = demuxer_->inner_streams_.find(type_);
  return it == demuxer_->inner_streams_.end() ? base::TimeDelta()
                                              : it->second->buffered_duration();
}

}  // namespace avbase::media

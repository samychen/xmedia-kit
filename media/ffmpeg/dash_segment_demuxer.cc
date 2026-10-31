// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Lifecycle, queries and the wrapper streams of DashSegmentDemuxer. The
// split mirrors HlsPlaylistDemuxer's: this TU owns "how the demuxer is
// opened and what it answers about itself", dash_segment_demuxer_read.cc
// owns "how a read crosses a segment boundary" plus the segment-open chain
// it feeds. Everything here runs on the media sequence except the worker
// halves and the thread-safe queries.

#include "media/ffmpeg/dash_segment_demuxer.h"

#include <algorithm>
#include <utility>

#include "base/functional/bind.h"
#include "base/task/sequenced_task_runner.h"
#include "media/ffmpeg/hls_fetch_utils.h"

namespace xmedia::media {
namespace {}  // namespace

// ---- lifecycle
// ---------------------------------------------------------------

DashSegmentDemuxer::DashSegmentDemuxer(base::scoped_refptr<MediaLog> media_log,
                                       BufferQueueLimits limits)
    : limits_(limits), media_log_(std::move(media_log)) {}

DashSegmentDemuxer::~DashSegmentDemuxer() {
  Stop();
}

void DashSegmentDemuxer::Initialize(
    const DataSourceDescriptor& source, const DemuxerOptions& options,
    Host* host,
    base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
    InitializeCB init_cb) {
  source_ = source;
  options_ = options;
  host_ = host;
  media_task_runner_ = std::move(media_task_runner);
  mpd_url_ = source_.uri;

  if (!worker_.Start()) {
    media_task_runner_->PostTask(
        FROM_HERE, base::BindOnce(std::move(init_cb),
                                  base::unexpected(MediaError::Of(
                                      ErrorCode::kSourceOpenFailed,
                                      "the dash worker thread failed to start",
                                      "url = " + mpd_url_,
                                      "check system thread limits"))));
    return;
  }
  phase_ = Phase::kInit;
  init_cb_ = std::move(init_cb);
  // The init callback lives ONLY in |init_cb_| from here on: threading it
  // through the worker callbacks would leave the success path holding a
  // moved-from callback (the double-move that null-checked here once).
  FetchMpd();
}

void DashSegmentDemuxer::FetchMpd() {
  worker_.task_runner()->PostTask(
      FROM_HERE, base::BindOnce(&DashSegmentDemuxer::FetchUrlOnWorker,
                                base::Unretained(this), mpd_url_,
      base::BindOnce(&DashSegmentDemuxer::OnMpdFetched,
                                               base::Unretained(this))));
}

void DashSegmentDemuxer::FetchUrlOnWorker(
    const std::string& url, base::OnceCallback<void(bool, std::string)> done) {
  std::string body;
  const bool ok = hls_fetch::FetchWholeUrl(url, &active_fetch_, &lock_, &body);
  media_task_runner_->PostTask(
      FROM_HERE, base::BindOnce(std::move(done), ok, std::move(body)));
}

void DashSegmentDemuxer::OnMpdFetched(bool ok, std::string body) {
  if (stopped_.load()) {
    return;
  }
  auto fail = [this](MediaError error) {
    auto cb = std::move(init_cb_);
    init_cb_ = InitializeCB();
    phase_ = Phase::kPlaying;  // nothing left to gate
    std::move(cb).Run(base::unexpected(std::move(error)));
  };
  if (!ok) {
    fail(MediaError::Of(ErrorCode::kSourceOpenFailed,
                        "the mpd could not be fetched", "url = " + mpd_url_,
                        "check the url and the server's response"));
    return;
  }
  auto parsed = dash::ParseMpd(body, mpd_url_);
  if (!parsed.has_value()) {
    fail(parsed.error());
    return;
  }
  mpd_ = std::move(*parsed);
  if (mpd_.is_live) {
    // Registered follow-up: dynamic MPDs reload their availability window;
    // the static path below assumes a fixed segment list.
    fail(MediaError::Of(ErrorCode::kSourceUnsupported,
                        "dynamic (live) dash is not built yet",
                        "url = " + mpd_url_,
                        "the manifest reload loop is the registered "
                        "follow-up to the dash demuxer integration; static "
                        "mpds play"));
    return;
  }
  if (mpd_.representations.empty()) {
    fail(MediaError::Of(ErrorCode::kSourceUnsupported,
                        "the mpd names no representation",
                        "url = " + mpd_url_,
                        "check that SegmentTemplate carries media and "
                        "duration"));
    return;
  }
  // Representation selection: the highest advertised bandwidth per type
  // (CicadaPlayer's default; ABR-driven switching is the follow-up that
  // revisits this). A rep whose mime says video covers the video chain, and
  // so on; a muxed rep (TS) covers several types once its inner opens.
  const dash::MpdRepresentation* video = nullptr;
  const dash::MpdRepresentation* audio = nullptr;
  for (const auto& rep : mpd_.representations) {
    if (rep.segment_urls.empty() || rep.segment_duration <= base::TimeDelta()) {
      continue;
    }
    if (rep.mime_type.rfind("video/", 0) == 0 &&
        (video == nullptr || rep.bandwidth > video->bandwidth)) {
      video = &rep;
    } else if (rep.mime_type.rfind("audio/", 0) == 0 &&
               (audio == nullptr || rep.bandwidth > audio->bandwidth)) {
      audio = &rep;
    }
  }
  if (video == nullptr && audio == nullptr) {
    fail(MediaError::Of(ErrorCode::kSourceUnsupported,
                        "the mpd has no video or audio representation",
                        "url = " + mpd_url_,
                        "check the AdaptationSet/Representation mime types"));
    return;
  }
  auto make_chain = [](const dash::MpdRepresentation& rep) {
    Chain chain;
    // mpd_parser puts the initialisation segment FIRST when the template
    // names one. The media count is what the durations add up to; any
    // surplus entry at the front of the resolved list is the init segment,
    // fetched once and prepended to every media body (fMP4 needs it, TS
    // lists simply have none).
    const size_t media_count = static_cast<size_t>(
        rep.total_duration / std::max(rep.segment_duration,
                                      base::TimeDelta::FromMicroseconds(1)));
    chain.media_duration = rep.segment_duration;
    if (media_count < rep.segment_urls.size() && media_count > 0) {
      chain.media_urls.assign(rep.segment_urls.begin() + 1,
                              rep.segment_urls.end());
      chain.init_pending_url = rep.segment_urls.front();
    } else {
      chain.media_urls = rep.segment_urls;
    }
    return chain;
  };
  if (video != nullptr) {
    chains_.push_back(make_chain(*video));
  }
  if (audio != nullptr) {
    chains_.push_back(make_chain(*audio));
  }
  if (chains_.size() == 2 &&
      chains_[0].media_urls.size() != chains_[1].media_urls.size()) {
    // The two chains must stay segment-aligned for the shared timeline;
    // real manifests align representations within a Period. Rather than
    // guess an alignment, play the video representation alone.
    chains_.resize(1);
  }

  init_chains_pending_ = chains_.size();
  for (size_t i = 0; i < chains_.size(); ++i) {
    OpenSegment(i, 0);
  }
}

// ---- queries
// -----------------------------------------------------------------

DemuxerStream* DashSegmentDemuxer::GetStream(DemuxerStreamType type) {
  auto it = streams_.find(type);
  return it == streams_.end() ? nullptr : it->second.get();
}

void DashSegmentDemuxer::SetActiveStream(DemuxerStreamType type,
                                         int stream_index) {
  Chain* chain = chain_for_type(type);
  if (chain != nullptr && chain->inner != nullptr) {
    chain->inner->SetActiveStream(type, stream_index);
  }
}

const MediaInfo& DashSegmentDemuxer::media_info() const {
  return media_info_;
}

base::TimeDelta DashSegmentDemuxer::GetStartTime() const {
  return base::TimeDelta();
}

bool DashSegmentDemuxer::IsLive() const {
  return false;  // dynamic MPDs are rejected at Initialize
}

bool DashSegmentDemuxer::IsSeekable() const {
  return true;
}

DemuxerStats DashSegmentDemuxer::GetStats() const {
  return stats_;
}

const char* DashSegmentDemuxer::name() const {
  return "DashSegmentDemuxer";
}

size_t DashSegmentDemuxer::GetBufferedPacketCount() const {
  size_t total = 0;
  for (auto& chain : chains_) {
    if (chain.inner != nullptr) {
      total += chain.inner->GetBufferedPacketCount();
    }
  }
  return total;
}

DashSegmentDemuxer::Chain* DashSegmentDemuxer::chain_for_type(
    DemuxerStreamType type) {
  auto it = type_owner_.find(type);
  return it == type_owner_.end() ? nullptr : &chains_[it->second];
}

size_t DashSegmentDemuxer::chain_index_for_type(
    DemuxerStreamType type) const {
  auto it = type_owner_.find(type);
  return it == type_owner_.end() ? 0 : it->second;
}

// ---- SegmentStream
// -------------------------------------------------------------

DashSegmentDemuxer::SegmentStream::SegmentStream(DashSegmentDemuxer* demuxer,
                                                 DemuxerStreamType type)
    : demuxer_(demuxer), type_(type) {}

DashSegmentDemuxer::SegmentStream::~SegmentStream() = default;

void DashSegmentDemuxer::SegmentStream::Read(uint32_t count, ReadCB read_cb) {
  demuxer_->ReadFromStream(type_, count, std::move(read_cb));
}

const AudioDecoderConfig&
DashSegmentDemuxer::SegmentStream::audio_decoder_config() const {
  static const AudioDecoderConfig empty;
  const Chain* chain = demuxer_->chain_for_type(type_);
  if (chain == nullptr || chain->inner == nullptr) {
    return empty;
  }
  auto it = chain->inner_streams.find(type_);
  return it == chain->inner_streams.end()
             ? empty
             : it->second->audio_decoder_config();
}

const VideoDecoderConfig&
DashSegmentDemuxer::SegmentStream::video_decoder_config() const {
  static const VideoDecoderConfig empty;
  const Chain* chain = demuxer_->chain_for_type(type_);
  if (chain == nullptr || chain->inner == nullptr) {
    return empty;
  }
  auto it = chain->inner_streams.find(type_);
  return it == chain->inner_streams.end()
             ? empty
             : it->second->video_decoder_config();
}

const TextDecoderConfig&
DashSegmentDemuxer::SegmentStream::text_decoder_config() const {
  static const TextDecoderConfig empty;
  const Chain* chain = demuxer_->chain_for_type(type_);
  if (chain == nullptr || chain->inner == nullptr) {
    return empty;
  }
  auto it = chain->inner_streams.find(type_);
  return it == chain->inner_streams.end()
             ? empty
             : it->second->text_decoder_config();
}

int32_t DashSegmentDemuxer::SegmentStream::stream_index() const {
  const Chain* chain = demuxer_->chain_for_type(type_);
  if (chain == nullptr || chain->inner == nullptr) {
    return 0;
  }
  auto it = chain->inner_streams.find(type_);
  return it == chain->inner_streams.end() ? 0 : it->second->stream_index();
}

StreamLiveness DashSegmentDemuxer::SegmentStream::liveness() const {
  return StreamLiveness::kRecorded;
}

bool DashSegmentDemuxer::SegmentStream::SupportsConfigChanges() const {
  const Chain* chain = demuxer_->chain_for_type(type_);
  if (chain == nullptr || chain->inner == nullptr) {
    return false;
  }
  auto it = chain->inner_streams.find(type_);
  return it != chain->inner_streams.end() &&
         it->second->SupportsConfigChanges();
}

int32_t DashSegmentDemuxer::SegmentStream::serial() const {
  return demuxer_->serial_;
}

size_t DashSegmentDemuxer::SegmentStream::buffered_buffers() const {
  const Chain* chain = demuxer_->chain_for_type(type_);
  if (chain == nullptr || chain->inner == nullptr) {
    return 0;
  }
  auto it = chain->inner_streams.find(type_);
  return it == chain->inner_streams.end() ? 0 : it->second->buffered_buffers();
}

size_t DashSegmentDemuxer::SegmentStream::buffered_bytes() const {
  const Chain* chain = demuxer_->chain_for_type(type_);
  if (chain == nullptr || chain->inner == nullptr) {
    return 0;
  }
  auto it = chain->inner_streams.find(type_);
  return it == chain->inner_streams.end() ? 0 : it->second->buffered_bytes();
}

base::TimeDelta DashSegmentDemuxer::SegmentStream::buffered_duration() const {
  const Chain* chain = demuxer_->chain_for_type(type_);
  if (chain == nullptr || chain->inner == nullptr) {
    return base::TimeDelta();
  }
  auto it = chain->inner_streams.find(type_);
  return it == chain->inner_streams.end()
             ? base::TimeDelta()
             : it->second->buffered_duration();
}

}  // namespace xmedia::media

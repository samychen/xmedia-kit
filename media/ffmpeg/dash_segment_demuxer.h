// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef XMEDIA_MEDIA_FFMPEG_DASH_SEGMENT_DEMUXER_H_
#define XMEDIA_MEDIA_FFMPEG_DASH_SEGMENT_DEMUXER_H_

#include <atomic>
#include <map>
#include <optional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/memory/raw_ptr.h"
#include "base/threading/thread.h"
#include "media/base/decoder_buffer_queue.h"
#include "media/base/demuxer.h"
#include "media/ffmpeg/ffmpeg_demuxer.h"
#include "media/ffmpeg/mpd_parser.h"

namespace xmedia::media {

// The DASH client demuxer (cicada-port T8's registered follow-up): an MPD
// fetched over the DataSource abstraction, representations selected from it,
// and each selected representation's segments opened as their own inner
// FFmpegDemuxer -- the same shape the HLS playlist demuxer pioneered, with
// one structural difference: HLS chains ONE segment sequence (muxed TS)
// across every stream, while DASH audio/video live in SEPARATE
// representations with separate segment sequences. The demuxer therefore
// keeps one chain per selected representation and binds each outer
// DemuxerStream to the chain whose inner provides its type.
//
// SUPPORTED. Static ("type=static") MPDs with SegmentTemplate expansion --
// exactly what mpd_parser resolves (initialisation segment prepended to
// every media segment body for fMP4; plain-TS segment lists need none). A
// muxed single representation (TS) covers several types from one chain, the
// same way a media playlist does.
//
// NOT YET (registered follow-ups, in PLAN §0's spirit): dynamic (live) MPDs
// with manifest reload -- Initialize rejects them with a structured error --
// multiple Periods, SegmentList/SegmentBase, rep switching mid-play, and
// UTCTiming clock sync.
//
// THREADING. Mirrors HlsPlaylistDemuxer: orchestration on |media_task_runner|
// (the sequence checker binds there through the inner demuxers), blocking
// fetches on a private worker thread, Stop() aborts the fetch in flight and
// answers every queued read kAborted. Queries are safe from any thread.
class XMEDIA_MEDIA_EXPORT DashSegmentDemuxer final : public Demuxer {
 public:
  DashSegmentDemuxer(base::scoped_refptr<MediaLog> media_log,
                     BufferQueueLimits limits);
  DashSegmentDemuxer(const DashSegmentDemuxer&) = delete;
  DashSegmentDemuxer& operator=(const DashSegmentDemuxer&) = delete;
  ~DashSegmentDemuxer() override;

  // ---- Demuxer ------------------------------------------------------------
  void Initialize(const DataSourceDescriptor& source,
                  const DemuxerOptions& options, Host* host,
                  base::scoped_refptr<base::SequencedTaskRunner>
                      media_task_runner,
                  InitializeCB init_cb) override;
  void StartPlayingFrom(base::TimeDelta time, SeekCB cb) override;
  void Flush(base::OnceClosure flush_cb) override;
  void Reset(base::OnceClosure reset_cb) override;
  void Stop() override;
  DemuxerStream* GetStream(DemuxerStreamType type) override;
  void SetActiveStream(DemuxerStreamType type, int stream_index) override;
  const MediaInfo& media_info() const override;
  base::TimeDelta GetStartTime() const override;
  bool IsLive() const override;
  bool IsSeekable() const override;
  DemuxerStats GetStats() const override;
  const char* name() const override;
  size_t GetBufferedPacketCount() const override;

 private:
  // One selected representation's segment chain. The inner FFmpegDemuxer is
  // rebuilt per segment (fetch -> concat init -> MemoryDataSource -> init);
  // |timeline| is the chain's media time at the START of media segment |i|,
  // i * segment_duration -- DASH SegmentTemplates are uniform, which is also
  // what keeps a video and an audio chain aligned without extra bookkeeping.
  struct Chain {
    std::vector<std::string> media_urls;  // resolved, media segments only
    // The initialisation segment's URL, before it is fetched; empty when the
    // template names none (TS segment lists). Once fetched the bytes live in
    // |init_bytes| and the URL is cleared.
    std::string init_pending_url;
    std::vector<uint8_t> init_bytes;
    base::TimeDelta media_duration;
    std::unique_ptr<FFmpegDemuxer> inner;
    std::map<DemuxerStreamType, DemuxerStream*> inner_streams;
    size_t cursor = 0;            // media segment currently open
    bool open_in_flight = false;  // fetch/init between segments
    bool ended = false;           // served its natural EOS
    // One delta per open segment, taken from the first timestamped buffer.
    base::TimeDelta segment_delta;
    bool segment_delta_set = false;
    base::TimeDelta timeline_start;
    std::string last_url;  // diagnostics on failure

    base::TimeDelta TimelineFor(size_t media_index) const {
      return media_duration * static_cast<int64_t>(media_index);
    }
  };

  // The pipeline-facing stream: forwards config/type to its chain's inner
  // stream and queues reads across that chain's segment transitions.
  class SegmentStream final : public DemuxerStream {
   public:
    SegmentStream(DashSegmentDemuxer* demuxer, DemuxerStreamType type);
    SegmentStream(const SegmentStream&) = delete;
    SegmentStream& operator=(const SegmentStream&) = delete;
    ~SegmentStream() override;

    // DemuxerStream:
    void Read(uint32_t count, ReadCB read_cb) override;
    const AudioDecoderConfig& audio_decoder_config() const override;
    const VideoDecoderConfig& video_decoder_config() const override;
    const TextDecoderConfig& text_decoder_config() const override;
    DemuxerStreamType type() const override { return type_; }
    int32_t stream_index() const override;
    StreamLiveness liveness() const override;
    bool SupportsConfigChanges() const override;
    int32_t serial() const override;
    size_t buffered_buffers() const override;
    size_t buffered_bytes() const override;
    base::TimeDelta buffered_duration() const override;

   private:
    friend class DashSegmentDemuxer;
    base::raw_ptr<DashSegmentDemuxer> demuxer_;
    DemuxerStreamType type_;
  };

  // A read queued while its chain is between segments.
  struct PendingRead {
    DemuxerStreamType type;
    uint32_t count;
    DemuxerStream::ReadCB read_cb;
  };

  // Unlike the single-chain HLS demuxer (one OpenCompletion per open), the
  // N-chain DASH shape funnels every segment open through a PHASE: kInit
  // waits for all chains' segment 0, kSeek waits for all chains' target
  // segment before answering the seek, kPlaying just re-issues the chain's
  // queued reads.
  enum class Phase { kInit, kPlaying, kSeek };

  // ---- open chain ---------------------------------------------------------
  void FetchMpd();
  void OnMpdFetched(bool ok, std::string body);
  // Opens |chain_index|'s media segment |index| (fetch -> concat -> inner).
  void OpenSegment(size_t chain_index, size_t segment_index);
  void OnSegmentBytesReady(size_t chain_index, size_t segment_index,
                           base::expected<std::vector<uint8_t>, MediaError>
                               bytes_or_error);
  void OnInnerSegmentInitialized(std::unique_ptr<FFmpegDemuxer> inner,
                                 size_t chain_index, size_t segment_index,
                                 Status status);
  void OnSegmentOpened(size_t chain_index, size_t segment_index, Status status);
  void FetchSegmentOnWorker(size_t chain_index, size_t segment_index,
                            std::string url);
  void FetchUrlOnWorker(const std::string& url,
                        base::OnceCallback<void(bool, std::string)> done);
  // The per-phase settle: kInit finishes Initialize when the last chain's
  // segment 0 opened (or fails it on the first real error); kSeek runs the
  // inners' StartPlayingFrom once every chain reopened.
  void OnChainSettled(size_t chain_index, Status status);
  void FinishInit(Status status);
  void RunSeekOnChain(size_t chain_index);
  void OnChainSeekDone(size_t chain_index, Status status,
                       base::TimeDelta actual);

  // ---- read orchestration (media sequence; dash_segment_demuxer_read.cc) --
  void ReadFromStream(DemuxerStreamType type, uint32_t count,
                      DemuxerStream::ReadCB read_cb);
  void OnStreamRead(DemuxerStreamType type, uint32_t count,
                    DemuxerStream::ReadCB read_cb,
                    DemuxerStream::Status status,
                    DemuxerStream::DecoderBufferVector buffers);
  void AdvanceChain(size_t chain_index);
  void SeekChainOpened(base::TimeDelta request, SeekCB cb);
  void CloseChainInner(size_t chain_index);
  Chain* chain_for_type(DemuxerStreamType type);
  size_t chain_index_for_type(DemuxerStreamType type) const;
  size_t SegmentIndexForTime(size_t chain_index, base::TimeDelta time) const;

  BufferQueueLimits limits_;
  base::scoped_refptr<MediaLog> media_log_;
  DataSourceDescriptor source_;
  DemuxerOptions options_;
  base::raw_ptr<Host> host_ = nullptr;
  base::scoped_refptr<base::SequencedTaskRunner> media_task_runner_;

  base::Thread worker_{"xmedia-dash"};
  std::mutex lock_;                 // guards |active_fetch_|
  base::raw_ptr<DataSource> active_fetch_ = nullptr;
  std::atomic<bool> stopped_{false};

  dash::Mpd mpd_;
  std::string mpd_url_;
  Phase phase_ = Phase::kInit;
  InitializeCB init_cb_;      // pending only during Phase::kInit
  SeekCB seek_cb_;            // pending only during Phase::kSeek
  base::TimeDelta seek_request_;
  std::optional<base::TimeDelta> seek_actual_;
  int seek_chains_pending_ = 0;
  int init_chains_pending_ = 0;
  bool init_failed_ = false;  // one real error fails the whole open
  std::vector<Chain> chains_;
  std::map<DemuxerStreamType, size_t> type_owner_;
  std::map<DemuxerStreamType, std::unique_ptr<SegmentStream>> streams_;
  // Reads queued while their chain is between segments, in arrival order.
  std::vector<PendingRead> pending_reads_;
  // The init-phase completion that waits for EVERY chain to open segment 0.
  size_t chains_pending_open_ = 0;

  int32_t serial_ = 0;
  MediaInfo media_info_;
  DemuxerStats stats_;
  bool initialized_ = false;
};

}  // namespace xmedia::media

#endif  // XMEDIA_MEDIA_FFMPEG_DASH_SEGMENT_DEMUXER_H_

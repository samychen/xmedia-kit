// Copyright 2026 The avbase Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef AVBASE_MEDIA_FFMPEG_HLS_PLAYLIST_DEMUXER_H_
#define AVBASE_MEDIA_FFMPEG_HLS_PLAYLIST_DEMUXER_H_

#include <stdint.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/thread.h"
#include "base/time/time.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/decoder_buffer_queue.h"
#include "media/base/demuxer.h"
#include "media/base/demuxer_stream.h"
#include "media/base/media_error.h"
#include "media/base/media_info.h"
#include "media/ffmpeg/hls_m3u8_parser.h"
#include "media/filters/abr_algorithm.h"
#include "media/filters/abr_bandwidth_estimator.h"
#include "media/media_export.h"

namespace avbase::media {

class FFmpegDemuxer;

// An HLS client demuxer -- the port of CicadaPlayer's playList_demuxer to
// avbase's Demuxer contract. The playlist (fetched and parsed with
// ParseM3U8) is the timeline; every segment is served by its own short-
// lived FFmpegDemuxer over an in-memory source, and ONE set of wrapper
// streams (created at Initialize) survives across every segment and variant
// switch, so the pipeline never sees the seams.
//
// TIMELINE. Segment i starts at the running sum of the EXTINF durations
// before it (Segment::timeline_start). Each buffer is rebased into that
// timeline with one per-segment delta taken from the first timestamped
// buffer the segment produces -- A/V share the TS clock, so one delta keeps
// both streams aligned.
//
// THREADING. Every Demuxer entry point, including Stop(), runs on the
// pipeline's media sequence (the same contract the pipeline already gives
// FFmpegDemuxer's callbacks); the wrapper streams are media-sequence
// objects. The ONLY cross-thread surface is the worker thread's fetch in
// flight, aborted through DataSource::Abort() under the fetch lock. Worker
// results come back as posts to the media sequence and are dropped when
// |stopped_| is set.
//
// SCOPE (docs/cicada-port/PLAN.md T3): VOD and live-with-reload; LL-HLS
// part prefetch and ABR-driven variant switching land with T4/T8 on the
// same seams (the variant list is already parsed; switching replaces the
// inner demuxer exactly like a segment transition).
class AVBASE_MEDIA_EXPORT HlsPlaylistDemuxer final : public Demuxer {
 public:
  explicit HlsPlaylistDemuxer(base::scoped_refptr<MediaLog> media_log,
                              BufferQueueLimits limits);
  HlsPlaylistDemuxer(const HlsPlaylistDemuxer&) = delete;
  HlsPlaylistDemuxer& operator=(const HlsPlaylistDemuxer&) = delete;
  ~HlsPlaylistDemuxer() override;

  // ---- Demuxer ------------------------------------------------------------
  void
  Initialize(const DataSourceDescriptor& source, const DemuxerOptions& options,
             Host* host,
             base::scoped_refptr<base::SequencedTaskRunner> media_task_runner,
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
  // What to do when the open of a segment settles. One shape for the three
  // callers of OpenSegment: the initial open (finishes Initialize), a
  // read-driven advance (re-issues the queued reads), and a seek landing in
  // another segment (finishes the inner StartPlayingFrom).
  struct OpenCompletion {
    enum class Kind { kNone, kInit, kSeek };
    Kind kind = Kind::kNone;
    InitializeCB init_cb;
    SeekCB seek_cb;
    base::TimeDelta seek_request;
  };

  // The pipeline-facing stream: forwards config/type to the current inner
  // stream and chains reads across segment transitions. All orchestration
  // (which segment is open, rebasing, serial generations) lives in the
  // demuxer; the stream is the stable identity the pipeline holds.
  class SegmentStream final : public DemuxerStream {
   public:
    SegmentStream(HlsPlaylistDemuxer* demuxer, DemuxerStreamType type);
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
    friend class HlsPlaylistDemuxer;
    base::raw_ptr<HlsPlaylistDemuxer> demuxer_;
    DemuxerStreamType type_;
  };

  // ---- open chain (playlist -> variant -> segment 0) -----------------------
  void FetchPlaylist(InitializeCB init_cb);
  void OnPlaylistFetched(InitializeCB init_cb, bool ok, std::string body);
  void OnVariantFetched(InitializeCB init_cb, const std::string& variant_url,
                        bool ok, std::string body);
  void OpenFirstSegment(InitializeCB init_cb);
  void OnFirstSegmentReady(Status status, InitializeCB init_cb);

  // ---- read orchestration (media sequence; see hls_playlist_demuxer_read.cc)
  void ReadFromStream(SegmentStream* stream, uint32_t count,
                      DemuxerStream::ReadCB read_cb);
  void OnStreamRead(SegmentStream* stream, uint32_t count,
                    DemuxerStream::ReadCB read_cb, DemuxerStream::Status status,
                    DemuxerStream::DecoderBufferVector buffers);
  // ABR (T4): the decision is made mid-segment with a real buffer signal
  // and executed at the boundary, which fetches the target rung's playlist
  // before its first segment.
  void MaybeDecideSwitch();
  void AdvanceAfterBoundary(OpenCompletion completion);
  void StartVariantSwitch(int variant_index, OpenCompletion completion);
  void OnVariantSwitchLoaded(int variant_index, bool ok, std::string body);
  int RungForVariant(int variant_index) const;
  void OpenSegment(size_t index, OpenCompletion completion);
  void OnSegmentOpened(size_t index, OpenCompletion completion, Status status);
  void OnInnerSegmentInitialized(std::unique_ptr<FFmpegDemuxer> inner,
                                 size_t index, Status status);
  void ReloadPlaylist(OpenCompletion completion);
  void OnPlaylistReloaded(OpenCompletion completion, bool ok, std::string body);
  void SeekSegmentOpened(base::TimeDelta request, SeekCB cb);
  size_t SegmentIndexForTime(base::TimeDelta time) const;
  void CloseInner();

  // Blocking halves, on the worker thread.
  void FetchUrlOnWorker(const std::string& url,
                        base::OnceCallback<void(bool, std::string)> done);
  void FetchSegmentOnWorker(size_t index);
  // Media sequence: owns the inner FFmpegDemuxer construction, which must
  // happen there -- its sequence checker binds to the constructing runner.
  void OnSegmentBytesReady(
      size_t index,
      base::expected<std::vector<uint8_t>, MediaError> bytes_or_error);

  const BufferQueueLimits limits_;
  base::scoped_refptr<MediaLog> media_log_;
  base::raw_ptr<Host> host_ = nullptr;
  base::scoped_refptr<base::SequencedTaskRunner> media_task_runner_;
  DataSourceDescriptor source_;
  DemuxerOptions options_;
  std::string playlist_url_;

  // Cross-thread: the worker sets it, Stop() aborts it. GUARDED_BY(lock_)
  // for the pointer itself.
  mutable std::mutex lock_;
  base::raw_ptr<DataSource> active_fetch_;
  std::atomic<bool> stopped_{false};

  // Media-sequence state. No lock: every writer and reader is sequenced by
  // |media_task_runner_| (the class comment records the contract).
  hls::Playlist playlist_;
  size_t cursor_ = 0;
  std::unique_ptr<FFmpegDemuxer> inner_;
  std::map<DemuxerStreamType, DemuxerStream*> inner_streams_;
  std::map<DemuxerStreamType, std::unique_ptr<SegmentStream>> streams_;
  base::TimeDelta segment_delta_;
  bool segment_delta_set_ = false;
  int32_t serial_ = 0;
  bool open_in_flight_ = false;
  MediaInfo media_info_;
  bool initialized_ = false;

  // Reads queued while a segment opens (or a live playlist reloads).
  struct PendingRead {
    base::raw_ptr<SegmentStream> stream;
    uint32_t count;
    DemuxerStream::ReadCB read_cb;
  };
  std::vector<PendingRead> pending_reads_;

  // Runs every blocking fetch (playlists, segments, keys).
  base::Thread worker_{"avbase-hls"};
  DemuxerStats stats_;
  // Cached AES key: one EXT-X-KEY per playlist in practice, so keep the
  // last one and reuse it across segments.
  std::string cached_key_url_;
  std::vector<uint8_t> cached_key_;
  // The completion carried across the worker hop (FetchSegmentOnWorker has
  // no return path; OnSegmentBytesReady picks it up on the media sequence).
  OpenCompletion open_completion_;

  // ---- ABR state (inactive without a master playlist) ----------------------
  // The master's variants sorted ascending by bandwidth; |active_rung_| is
  // the position in that ladder of the playlist currently being served.
  std::vector<hls::Variant> abr_ladder_;       // ascending by bandwidth
  std::vector<hls::Variant> master_variants_;  // master order
  int active_variant_ = -1;  // index into the MASTER's variant list
  int active_rung_ = -1;     // index into |abr_ladder_|
  int active_variant_pending_ = -1;
  int pending_switch_rung_ = -1;
  bool switch_decided_ = false;
  AbrBandwidthEstimator abr_estimator_;
  std::unique_ptr<AbrAlgoStrategy> abr_algo_;
  // The open segment's bytes. MemoryDataSource does NOT copy, and the inner
  // demux thread keeps reading them for the whole segment's life, so the
  // buffer must outlive |inner_|; it is replaced only after CloseInner().
  std::vector<uint8_t> open_segment_bytes_;
};

}  // namespace avbase::media

#endif  // AVBASE_MEDIA_FFMPEG_HLS_PLAYLIST_DEMUXER_H_

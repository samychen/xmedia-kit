// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// One-to-one mapping from legacy's FFP_MSG_* constants; see docs/05 table 5.

#ifndef XMEDIA_PLAYER_PUBLIC_PLAYER_EVENT_H_
#define XMEDIA_PLAYER_PUBLIC_PLAYER_EVENT_H_

#include <stdint.h>

#include <string>
#include <variant>
#include <vector>

#include "base/time/time.h"
#include "media/base/media_types.h"
#include "media/base/video_frame.h"
#include "player/public/error.h"
#include "player/public/media_info.h"
#include "player/public/playback_stats.h"
#include "player/public/player_config.h"
#include "player/public/player_export.h"

namespace xmedia {

enum class PlayerState : uint8_t {
  kIdle = 0,
  kInitialized,
  kPreparing,
  kPrepared,
  kStarted,
  kPaused,
  kBuffering,
  kStopping,
  kStopped,
  kCompleted,
  kError,
  kEnd,
};
// Stable spelling of |state| for logs and diagnostics; never null.
// Side effects: none.
XMEDIA_PLAYER_EXPORT const char* GetPlayerStateName(PlayerState state);

enum class EventType {
  kPrepared,
  kCompleted,
  kError,
  kFlushed,
  kStateChanged,
  kVideoSizeChanged,
  kRotationChanged,
  kRenderingStarted,
  kDecodingStarted,
  kDecoderOpened,
  kDecoderFallback,
  kStageReached,
  kWaiting,
  kBufferingStarted,
  kBufferingEnded,
  kBufferingProgress,
  kBufferedBytesUpdate,
  kBufferedTimeUpdate,
  kSeekCompleted,
  kAccurateSeekCompleted,
  kSeekRenderingStarted,
  kTimedText,
  kTrackChanged,
  kSnapshotCompleted,
  kStats,
  kHdrUnsupported,
  kUtcTime,
};
// Stable spelling of |type| for logs and diagnostics; never null.
// Side effects: none.
XMEDIA_PLAYER_EXPORT const char* GetEventTypeName(EventType type);

struct XMEDIA_PLAYER_EXPORT PreparedPayload {
  MediaInfo media_info;
  StageTimings stages;
};
struct XMEDIA_PLAYER_EXPORT ErrorPayload {
  MediaError error;
};
struct XMEDIA_PLAYER_EXPORT StateChangedPayload {
  PlayerState from{PlayerState::kIdle};
  PlayerState to{PlayerState::kIdle};
};
// Merges legacy's FFP_MSG_VIDEO_SIZE_CHANGED (400) and SAR_CHANGED (401),
// which could arrive out of order (behaviour difference Δ8).
struct XMEDIA_PLAYER_EXPORT VideoSizeChangedPayload {
  media::Size coded_size;
  media::Size natural_size;
  media::Rational sar{1, 1};
};
struct XMEDIA_PLAYER_EXPORT RotationChangedPayload {
  int degrees{0};
  bool mirrored{false};
};
struct XMEDIA_PLAYER_EXPORT RenderingStartedPayload {
  media::DemuxerStreamType type{media::DemuxerStreamType::kUnknown};
};
struct XMEDIA_PLAYER_EXPORT DecodingStartedPayload {
  media::DemuxerStreamType type{media::DemuxerStreamType::kUnknown};
  media::VideoDecoderType video_type{media::VideoDecoderType::kUnknown};
  media::AudioDecoderType audio_type{media::AudioDecoderType::kUnknown};
  std::string codec_name;
};
struct XMEDIA_PLAYER_EXPORT DecoderOpenedPayload {
  media::DemuxerStreamType type{media::DemuxerStreamType::kUnknown};
  std::string decoder_name;
  bool outputs_opaque_surface{false};
};
struct XMEDIA_PLAYER_EXPORT DecoderFallbackPayload {
  std::string from_decoder;
  std::string to_decoder;
  std::string reason;
};
struct XMEDIA_PLAYER_EXPORT StageReachedPayload {
  enum class Stage {
    kOpenInput,
    kFindStreamInfo,
    kComponentOpen
  } stage{Stage::kOpenInput};
  base::TimeDelta elapsed_since_prepare;
};
struct XMEDIA_PLAYER_EXPORT WaitingPayload {
  enum class Reason {
    kBuffering,
    kDecoderStalled,
    kNetworkStalled,
    kKeyRequired
  } reason{Reason::kBuffering};
};
struct XMEDIA_PLAYER_EXPORT BufferingProgressPayload {
  base::TimeDelta buffered_ahead;
  int percent{0};
};
struct XMEDIA_PLAYER_EXPORT BufferedUpdatePayload {
  base::TimeDelta cached_time;
  base::TimeDelta high_water_mark_time;
  size_t cached_bytes{0};
  size_t high_water_mark_bytes{0};
};
struct XMEDIA_PLAYER_EXPORT SeekCompletedPayload {
  int64_t request_id{0};  // Matches the SeekTo() call; see Δ.
  base::TimeDelta requested;
  base::TimeDelta actual;
  base::TimeDelta load_duration;
  MediaError result;
};
struct XMEDIA_PLAYER_EXPORT AccurateSeekCompletedPayload {
  base::TimeDelta position;
};
struct XMEDIA_PLAYER_EXPORT SeekRenderingStartedPayload {
  media::DemuxerStreamType type{media::DemuxerStreamType::kUnknown};
};
struct XMEDIA_PLAYER_EXPORT TimedTextPayload {
  std::string text;
  base::TimeDelta pts;
  base::TimeDelta duration;
  int x{0}, y{0}, w{0}, h{0};
  std::vector<uint8_t> raw_ass;  // Raw ASS packet for hosts that do layout.
};
struct XMEDIA_PLAYER_EXPORT TrackChangedPayload {
  media::DemuxerStreamType type{media::DemuxerStreamType::kUnknown};
  int old_index{-1};
  int new_index{-1};
};
struct XMEDIA_PLAYER_EXPORT SnapshotCompletedPayload {
  base::TimeDelta at;
  std::string file_path;
  MediaError result;
};
struct XMEDIA_PLAYER_EXPORT StatsPayload {
  PlaybackStats stats;
};
struct XMEDIA_PLAYER_EXPORT FlushedPayload {};
struct XMEDIA_PLAYER_EXPORT CompletedPayload {};
struct XMEDIA_PLAYER_EXPORT HdrUnsupportedPayload {
  std::string transfer;
};
// Wall-clock anchor from a source that carries one (HLS
// EXT-X-PROGRAM-DATE-TIME): at media position |anchor_media_time| the stream
// showed wall clock |epoch_ms|. Hosts extrapolate linearly
// (now_media_time - anchor) for the current UTC; the Player's
// GetUtcTimeMs() does exactly that with its own media clock.
struct XMEDIA_PLAYER_EXPORT UtcTimePayload {
  int64_t epoch_ms{0};
  int64_t anchor_media_time_us{0};
};

using EventPayload = std::variant<
    PreparedPayload, CompletedPayload, FlushedPayload, ErrorPayload,
    StateChangedPayload, VideoSizeChangedPayload, RotationChangedPayload,
    RenderingStartedPayload, DecodingStartedPayload, DecoderOpenedPayload,
    DecoderFallbackPayload, StageReachedPayload, WaitingPayload,
    BufferingProgressPayload, BufferedUpdatePayload, SeekCompletedPayload,
    AccurateSeekCompletedPayload, SeekRenderingStartedPayload, TimedTextPayload,
    TrackChangedPayload, SnapshotCompletedPayload, StatsPayload,
    HdrUnsupportedPayload, UtcTimePayload>;

struct XMEDIA_PLAYER_EXPORT PlayerEvent {
  EventType type{EventType::kError};
  EventPayload payload;
  base::TimeTicks wall_time;   // When the event was produced.
  base::TimeDelta media_time;  // Media position at production time.
  int64_t sequence_number{0};  // Monotonic; detects dropped events.
};

// Typed accessors. Each returns nullptr when |event| carries a different
// payload, so a switch on event.type plus one accessor is all a caller needs
// and std::visit never appears in user code (design rule E7).
#define XMEDIA_DECLARE_EVENT_ACCESSOR(Name, Type) \
  XMEDIA_PLAYER_EXPORT const Type* Name(const PlayerEvent& event)

XMEDIA_DECLARE_EVENT_ACCESSOR(AsPrepared, PreparedPayload);
XMEDIA_DECLARE_EVENT_ACCESSOR(AsUtcTime, UtcTimePayload);
XMEDIA_DECLARE_EVENT_ACCESSOR(AsError, ErrorPayload);
XMEDIA_DECLARE_EVENT_ACCESSOR(AsStateChanged, StateChangedPayload);
XMEDIA_DECLARE_EVENT_ACCESSOR(AsVideoSizeChanged, VideoSizeChangedPayload);
XMEDIA_DECLARE_EVENT_ACCESSOR(AsRotationChanged, RotationChangedPayload);
XMEDIA_DECLARE_EVENT_ACCESSOR(AsRenderingStarted, RenderingStartedPayload);
XMEDIA_DECLARE_EVENT_ACCESSOR(AsDecodingStarted, DecodingStartedPayload);
XMEDIA_DECLARE_EVENT_ACCESSOR(AsStageReached, StageReachedPayload);
XMEDIA_DECLARE_EVENT_ACCESSOR(AsBufferingProgress, BufferingProgressPayload);
XMEDIA_DECLARE_EVENT_ACCESSOR(AsBufferedUpdate, BufferedUpdatePayload);
XMEDIA_DECLARE_EVENT_ACCESSOR(AsSeekCompleted, SeekCompletedPayload);
XMEDIA_DECLARE_EVENT_ACCESSOR(AsAccurateSeekCompleted,
                              AccurateSeekCompletedPayload);
XMEDIA_DECLARE_EVENT_ACCESSOR(AsTimedText, TimedTextPayload);
XMEDIA_DECLARE_EVENT_ACCESSOR(AsTrackChanged, TrackChangedPayload);
XMEDIA_DECLARE_EVENT_ACCESSOR(AsSnapshotCompleted, SnapshotCompletedPayload);
XMEDIA_DECLARE_EVENT_ACCESSOR(AsStats, StatsPayload);
#undef XMEDIA_DECLARE_EVENT_ACCESSOR

// Downgrade path for hosts that still speak legacy's (what, arg1, arg2, obj)
// quadruple — used by the optional C ABI layer and the Android JNI binding so
// that an existing Java message handler keeps working unchanged.
// Side effects: none (pure mapping; |obj| in the result borrows the event's
// strings and stays valid while |event| does).
struct XMEDIA_PLAYER_EXPORT LegacyEvent {
  int what{0};
  int64_t arg1{0};
  int64_t arg2{0};
  std::string obj;
};
// Maps a modern PlayerEvent to the legacy Android-style event the struct
// above describes, so an existing Java message handler keeps working.
// Thread safety: any thread; pure mapping, no shared state.
// Errors: returns false (and leaves |out| defaulted) when |event| has no
// legacy equivalent. Side effects: none; |out| borrows the event's strings
// and stays valid only while |event| does.
XMEDIA_PLAYER_EXPORT bool ToLegacyEvent(const PlayerEvent& event,
                                        LegacyEvent* out);

// Observer interface, for projects that prefer virtual dispatch over a
// std::function. Only the events you override are delivered.
//
// Every callback runs on the event dispatch sequence and must not block;
// the default implementations ignore the event. Overriding OnEvent() alone
// receives everything (including the events without a dedicated hook).
class XMEDIA_PLAYER_EXPORT PlayerObserver {
 public:
  PlayerObserver(const PlayerObserver&) = delete;
  PlayerObserver& operator=(const PlayerObserver&) = delete;
  virtual ~PlayerObserver() = default;

  // Metadata is ready; the payload mirrors xmedia::MediaInfo.
  virtual void OnPrepared(const PreparedPayload&) {}
  // Playback reached the end (one call per pass with SetLoopCount).
  virtual void OnCompleted(const CompletedPayload&) {}
  // A fatal error; the payload carries the ErrorCode and a message.
  virtual void OnError(const ErrorPayload&) {}
  // The machine changed state; the payload names the transition.
  virtual void OnStateChanged(const StateChangedPayload&) {}
  // The video size (or rotation) changed; re-layout the surface.
  virtual void OnVideoSizeChanged(const VideoSizeChangedPayload&) {}
  // Buffering progress; the payload carries buffered-ahead and percent.
  virtual void OnBufferingProgress(const BufferingProgressPayload&) {}
  // A seek finished; the payload carries requested-vs-actual.
  virtual void OnSeekCompleted(const SeekCompletedPayload&) {}
  // A subtitle cue is due; the payload carries the cue text.
  virtual void OnTimedText(const TimedTextPayload&) {}
  // Periodic stats snapshot (see PlaybackStats).
  virtual void OnStats(const StatsPayload&) {}
  // Catch-all for anything not overridden above.
  virtual void OnEvent(const PlayerEvent&) {}

 protected:
  PlayerObserver() = default;
};

}  // namespace xmedia

#endif  // XMEDIA_PLAYER_PUBLIC_PLAYER_EVENT_H_

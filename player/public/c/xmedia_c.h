// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// xmedia-kit C ABI -- docs/08 M15, answers Q12.
//
// WHY THIS EXISTS
// The C++ API in player/public/ is the primary surface and stays that way: it
// carries std::string, Result<T> and move-only types that a C compiler cannot
// even parse. This header is a strictly NARROWER door to the same engine, for
// the hosts that cannot link C++ at all -- JNI (Android), Objective-C/Swift
// (iOS/macOS), and plain C hosts. Per docs/05 the goal is a zero-change
// migration for an existing ijkplayer's mEventHandler, so the event channel
// deliberately speaks the legacy (what, arg1, arg2, obj) quadruple rather than
// inventing a new event vocabulary.
//
// WHAT THIS IS NOT
// It is NOT a second implementation and NOT a subset with its own semantics.
// Every call below forwards to the same xmedia::Player. Where the C++ API has
// an answer, this file either forwards it verbatim or deliberately does not
// expose it -- with the reason written next to it. A C ABI that silently
// diverges from its C++ original is worse than no C ABI, because the bug then
// only reproduces on one side of the boundary.
//
// COMPATIBILITY RULES (binding for any future edit here)
//   1. Opaque handles only. xmedia_player* and friends are never dereferenced.
//   2. Layout is frozen. All structs are opaque; POD structs that cross the
//      boundary are append-only, and a new field is only legal at the end.
//   3. int32 codes, never C++ enums by value in a signature -- an enum's
//      underlying width is a compiler decision, not a contract.
//   4. Every string returned by this ABI is owned by the library, is NUL
//      terminated, and stays valid until the owning handle is destroyed. Copy
//      it if you need to keep it. No free() is exported: that would be a
//      cross-module allocator pairing, and mismatching them is a crash that
//      only reproduces in a release build.
//   5. No exceptions cross the boundary. Every entry point catches; a failure
//      becomes an XmediaStatus with a code, never a C++ exception unwinding
//      through a C frame (which is undefined behaviour, not "an error").
//   6. Thread safety matches the C++ API: per-player methods are callable from
//      any thread; the event callback runs on the event sequence.
//   7. No entry point blocks the caller, with two documented exceptions --
//      xmedia_player_stop_sync and xmedia_player_prepare_sync -- which wait
//      for the pipeline and are bounded by their timeout argument. Every
//      other call returns immediately; work happens on engine sequences and
//      arrives through the event callback.

#ifndef XMEDIA_PLAYER_PUBLIC_C_XMEDIA_C_H_
#define XMEDIA_PLAYER_PUBLIC_C_XMEDIA_C_H_

#include <stdint.h>

#include "player/public/c/xmedia_c_export.h"

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Version
// ---------------------------------------------------------------------------

// Version of the C ABI itself, not of the engine. Bumped ONLY when an existing
// symbol changes meaning or disappears; adding symbols is a minor bump. A host
// compiled against an older header keeps working against a newer library, so
// this exists to let it decide whether to feature-detect instead.
// Side effects: none.
XMEDIA_C_API const char* xmedia_c_version(void);

// Mirrors xmedia::GetVersion() ("0.1.0").
// Side effects: none.
XMEDIA_C_API const char* xmedia_engine_version(void);

// Mirrors xmedia::GlobalInit(). Optional: every xmedia_player_create() calls
// it. Call it yourself only to guarantee it happens before your first X11 or
// FFmpeg call.
// Side effects: idempotent global init (FFmpeg/X11 once per process).
XMEDIA_C_API void xmedia_global_init(void);

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

// Mirrors xmedia::ErrorCode as a flat int. The C++ enum is a uint16_t list of
// 30 values; the numeric values are part of this ABI, so new codes may only be
// APPENDED here.
typedef int32_t XmediaStatus;

// kOk -- also the only value for which the other out-params are meaningful.
#define XMEDIA_OK 0
// Generic failure. Prefer the specific codes: a host that can tell "file not
// found" from "codec unsupported" can show the user something useful.
#define XMEDIA_ERR_FAILED 1
#define XMEDIA_ERR_INVALID_ARGUMENT 2
#define XMEDIA_ERR_INVALID_STATE 3
#define XMEDIA_ERR_NOT_IMPLEMENTED 4
#define XMEDIA_ERR_OUT_OF_MEMORY 5
#define XMEDIA_ERR_ABORTED 6
#define XMEDIA_ERR_TIMEOUT 7
#define XMEDIA_ERR_SOURCE_OPEN_FAILED 8
#define XMEDIA_ERR_SOURCE_NOT_FOUND 9
#define XMEDIA_ERR_SOURCE_READ_FAILED 10
#define XMEDIA_ERR_DECODER_NOT_FOUND 11
#define XMEDIA_ERR_DECODER_OPEN_FAILED 12
#define XMEDIA_ERR_DECODE_FAILED 13
#define XMEDIA_ERR_DECODER_UNSUPPORTED_CODEC 14
#define XMEDIA_ERR_SINK_NOT_ATTACHED 15
#define XMEDIA_ERR_STREAM_NOT_FOUND 16
#define XMEDIA_ERR_TRACK_NOT_FOUND 17
#define XMEDIA_ERR_CONFIG_INVALID 18
#define XMEDIA_ERR_EOS 19

// Stable, never-null, never-freed name of a code, e.g. "kSourceNotFound".
// The name is the enum SPELLING on purpose: it is what a bug report will say,
// and it stays readable across a mapping to a legacy code in a Java handler.
// Side effects: none.
XMEDIA_C_API const char* xmedia_status_name(XmediaStatus status);

// The engine's own message for |status|: a human sentence naming the likely
// cause, or "" when it has nothing to add. Never null.
// Side effects: none.
XMEDIA_C_API const char* xmedia_status_message(XmediaStatus status);

// ---------------------------------------------------------------------------
// Player
// ---------------------------------------------------------------------------

// Opaque. Create with xmedia_player_create(); destroy with
// xmedia_player_destroy(), which is bounded by PlayerConfig's shutdown
// timeout and therefore cannot hang the caller.
typedef struct XmediaPlayer XmediaPlayer;

// An opaque, host-owned window handle. Mirrors the kinds in
// media/base/native_display.h; see NativeDisplay::FromX11Window et al.
typedef struct XmediaNativeDisplay XmediaNativeDisplay;

// An opaque handle for a secondary media source registered with
// xmedia_player_add_source(). Stable across Stop/Reset.
typedef struct XmediaSource XmediaSource;

// Creates a player with the default platform backends and an event handler
// already attached, so no call can race a subscription. Returns null only when
// the process is out of memory.
// Side effects: allocates a player (release with xmedia_player_destroy).
XMEDIA_C_API XmediaPlayer* xmedia_player_create(void);

// Destroying a null player is a no-op (not an error) so that cleanup paths can
// call it unconditionally after a failed create.
// Side effects: releases every pipeline resource, bounded by the shutdown
// timeout.
XMEDIA_C_API void xmedia_player_destroy(XmediaPlayer* player);

// Bounded by PlayerConfig::shutdown_timeout; does not hang.
// Blocking Stop: returns once the pipeline has really stopped.
// Side effects: emits kStateChanged(kStopped).
XMEDIA_C_API void xmedia_player_stop_sync(XmediaPlayer* player,
                                          int64_t timeout_ms);

// SetDataSource(uri). |uri| is copied; the caller keeps ownership.
// Side effects: moves the machine to kInitialized.
XMEDIA_C_API XmediaStatus xmedia_player_set_data_source(XmediaPlayer* player,
                                                        const char* uri);

// PrepareAsync(): setup starts and this returns immediately; kPrepared (what
// =200) arrives through the event callback.
// Side effects: emits kPrepared, or kError on failure.
XMEDIA_C_API XmediaStatus xmedia_player_prepare(XmediaPlayer* player);
// Blocking PrepareAsync, bounded by |timeout_ms|; a successful return may
// already be kStarted when start_on_prepared is set.
// Side effects: emits kPrepared, or kError on failure.
XMEDIA_C_API XmediaStatus xmedia_player_prepare_sync(XmediaPlayer* player,
                                                     int64_t timeout_ms);

// Starts playback (legal from kPrepared/kPaused/kCompleted); illegal states
// are logged and ignored, never fatal.
// Side effects: emits kStateChanged(kStarted).
XMEDIA_C_API void xmedia_player_start(XmediaPlayer* player);
// Pauses a running player (legal from kStarted only).
// Side effects: emits kStateChanged(kPaused).
XMEDIA_C_API void xmedia_player_pause(XmediaPlayer* player);
// Stops playback and tears the pipeline down; idempotent.
// Side effects: emits kStateChanged(kStopped).
XMEDIA_C_API void xmedia_player_stop(XmediaPlayer* player);

// Back to kIdle and reusable. Releases the current media.
// Side effects: emits kStateChanged(kIdle); drops every buffered packet.
XMEDIA_C_API void xmedia_player_reset(XmediaPlayer* player);

// Single-frame advance while paused.
// Side effects: emits kSeekRenderingStarted for the stepped frame.
XMEDIA_C_API void xmedia_player_step_once(XmediaPlayer* player);

// Fire-and-forget seek; completion arrives as an event. |position_ms| is
// relative to the media start. Deliberately no return value: the C++ API's
// Result<int64_t> exists to hand back a request id for correlation, and that
// correlation is already carried by the event payload's request_id -- a host
// that needs it should use the richer C++ API.
// Side effects: emits kSeekCompleted (what=600, arg1 = actual ms).
XMEDIA_C_API void xmedia_player_seek_to(XmediaPlayer* player,
                                        int64_t position_ms);

// |rate| is clamped to [0.25, 4.0] as the C++ API does.
// Side effects: takes effect on the next rendered frame.
XMEDIA_C_API void xmedia_player_set_playback_rate(XmediaPlayer* player,
                                                  double rate);

// |volume| is clamped to [0.0, 1.0].
// Side effects: applies to the audio sink immediately.
XMEDIA_C_API void xmedia_player_set_volume(XmediaPlayer* player, double volume);
// Mutes or unmutes without touching the volume level.
// Side effects: applies to the audio sink immediately.
XMEDIA_C_API void xmedia_player_set_muted(XmediaPlayer* player, int muted);

// |count| < 0 means infinite.
// Side effects: emits kCompleted (what=300) once per finished pass.
XMEDIA_C_API void xmedia_player_set_loop_count(XmediaPlayer* player, int count);

// |stream_type| is an XMEDIA_STREAM_* value below. |stream_index| is the index
// reported by the prepared media, NOT an xmedia_source index.
// Side effects: emits kTrackChanged; decoders are rebuilt lazily.
XMEDIA_C_API XmediaStatus xmedia_player_select_track(XmediaPlayer* player,
                                                     int stream_type,
                                                     int stream_index);

// Attaches |display| as the video output, or detaches when null (audio keeps
// playing -- the documented background-playback case). The player holds a
// reference; destroying the display handle afterwards is safe.
// Side effects: re-targets the video sink on the next frame.
XMEDIA_C_API void xmedia_player_set_video_surface(XmediaPlayer* player,
                                                  XmediaNativeDisplay* display);

// Writes the frame at |at_ms| to |file_path| (PNG); completion arrives as
// kSnapshotCompleted (what=1000, arg2 = Status).
// Side effects: writes one file; emits kSnapshotCompleted.
XMEDIA_C_API XmediaStatus xmedia_player_take_snapshot(XmediaPlayer* player,
                                                      int64_t at_ms,
                                                      const char* file_path);

// Forces the network retry path now. Replaces ijkplayer's write-only
// FFP_PROP_INT64_IMMEDIATE_RECONNECT property.
// Side effects: re-runs the reconnect ladder.
XMEDIA_C_API void xmedia_player_reconnect_now(XmediaPlayer* player);

// ---- Queries: lock-free snapshots, callable from any thread ----------------

// Mirrors PlayerState. See XMEDIA_STATE_* below.
// Side effects: none.
XMEDIA_C_API int xmedia_player_state(const XmediaPlayer* player);
// Current playback position, including any seek in flight.
// Side effects: none.
XMEDIA_C_API int64_t xmedia_player_media_time_ms(const XmediaPlayer* player);
// How much media is buffered ahead of the playhead.
// Side effects: none.
XMEDIA_C_API int64_t xmedia_player_buffered_time_ms(const XmediaPlayer* player);
// Total duration in ms, or zero for a live source.
// Side effects: none.
XMEDIA_C_API int64_t xmedia_player_duration_ms(const XmediaPlayer* player);
// Nonzero when the source is live (no seekable timeline).
// Side effects: none.
XMEDIA_C_API int xmedia_player_is_live(const XmediaPlayer* player);
// Nonzero while playback is actively running.
// Side effects: none.
XMEDIA_C_API int xmedia_player_is_playing(const XmediaPlayer* player);
// Display width after rotation/aspect corrections.
// Side effects: none.
XMEDIA_C_API int32_t xmedia_player_video_width(const XmediaPlayer* player);
// Display height after rotation/aspect corrections.
// Side effects: none.
XMEDIA_C_API int32_t xmedia_player_video_height(const XmediaPlayer* player);
// Clockwise rotation the host must apply, in degrees.
// Side effects: none.
XMEDIA_C_API int32_t xmedia_player_video_rotation(const XmediaPlayer* player);
// Effective rate, after clamping.
// Side effects: none.
XMEDIA_C_API double xmedia_player_playback_rate(const XmediaPlayer* player);
// Effective volume, after clamping.
// Side effects: none.
XMEDIA_C_API double xmedia_player_volume(const XmediaPlayer* player);
// Current mute state.
// Side effects: none.
XMEDIA_C_API int xmedia_player_muted(const XmediaPlayer* player);

// Copies at most |capacity| bytes (including the NUL) of the human-readable
// media title into |out| and returns the number of bytes the full string needs,
// excluding the NUL. A return value >= |capacity| means it was truncated, so
// the caller can size a buffer and retry -- guessing a size is how the legacy
// path used to truncate paths silently.
// Side effects: none.
XMEDIA_C_API int32_t xmedia_player_title(XmediaPlayer* player, char* out,
                                         int32_t capacity);

// Full internal state as JSON (~1 ms). The first thing to attach to a bug
// report. Same copy-into-your-buffer contract as the title.
// Side effects: none.
XMEDIA_C_API int32_t xmedia_player_dump_diagnostics(const XmediaPlayer* player,
                                                    char* out,
                                                    int32_t capacity);

// ---------------------------------------------------------------------------
// Media info
// ---------------------------------------------------------------------------

// Fills a caller-allocated buffer with the media's duration, dimensions and
// track counts. A snapshot, not a live view. |out| may be null only if all
// pointers passed in are null (a no-op way to probe support).
typedef struct XmediaMediaInfo {
  int64_t duration_ms;
  int64_t bitrate;
  // Live streams report a zero duration, so this flag -- not the duration --
  // is how a host tells them apart.
  int is_live;
  int seekable;
  int32_t video_width;
  int32_t video_height;
  int32_t video_rotation;
  // Counts, so a host can size a track menu without parsing anything. Taken
  // from the FIRST stream of each kind for the size/rotation fields.
  int32_t video_track_count;
  int32_t audio_track_count;
  int32_t subtitle_track_count;
} XmediaMediaInfo;

// Available only after xmedia_player_prepare() has reported kPrepared;
// returns XMEDIA_ERR_INVALID_STATE before that.
// Side effects: none.
XMEDIA_C_API XmediaStatus
xmedia_player_get_media_info(const XmediaPlayer* player, XmediaMediaInfo* out);

// ---------------------------------------------------------------------------
// Multi-source composition
// ---------------------------------------------------------------------------

// Registers a SECONDARY source and returns its layer handle, or null when the
// player is no longer in a state that accepts one. Call before prepare().
//
// The returned handle stays valid across stop/reset (the scene keeps it) but
// must be released with xmedia_source_release(). Source 0 is the player's own
// xmedia_player_set_data_source(); each call adds the next id.
// Side effects: allocates a scene layer visible after the next composite.
XMEDIA_C_API XmediaSource* xmedia_player_add_source(XmediaPlayer* player,
                                                    const char* uri,
                                                    const char* name,
                                                    int32_t z_order);

// Releases a source handle. Null is a no-op. Does not unregister the source;
// it only drops this reference (matching the C++ shared_ptr semantics).
// Side effects: none (the source outlives the handle).
XMEDIA_C_API void xmedia_source_release(XmediaSource* source);

// Re-points a registered source at a new URI; takes effect at the next
// prepare.
// Side effects: invalidates the source's demuxer until re-prepared.
XMEDIA_C_API XmediaStatus xmedia_source_set_uri(XmediaSource* source,
                                                const char* uri);

// ---------------------------------------------------------------------------
// Native display
// ---------------------------------------------------------------------------

// Wrappers over media::NativeDisplay. Each takes the raw handle of that window
// system as an opaque pointer -- deliberately NOT typed, so that including
// this header never drags X11/Wayland/SDL/Win32 headers into a C host.

// X11: |display| is a `Display*`, |window| is an XID. The pointers are copied,
// not retained: the window must outlive the display handle.
// Side effects: allocates a display handle (release with
// xmedia_display_release).
XMEDIA_C_API XmediaNativeDisplay* xmedia_display_from_x11(void* x_display,
                                                          uintptr_t window);

// Wayland: |surface| is a `wl_surface*`. Wayland cannot report a surface size,
// so |width|/|height| must be supplied by the embedder.
// Side effects: allocates a display handle.
XMEDIA_C_API XmediaNativeDisplay* xmedia_display_from_wayland(void* wl_display,
                                                              void* wl_surface,
                                                              int32_t width,
                                                              int32_t height);

// SDL2: |sdl_window| is an `SDL_Window*`.
// Side effects: allocates a display handle.
XMEDIA_C_API XmediaNativeDisplay* xmedia_display_from_sdl2(void* sdl_window);
// GLFW: |glfw_window| is a `GLFWwindow*`.
// Side effects: allocates a display handle.
XMEDIA_C_API XmediaNativeDisplay* xmedia_display_from_glfw(void* glfw_window);
// EGL: |egl_surface| is an `EGLSurface` as an opaque pointer.
// Side effects: allocates a display handle.
XMEDIA_C_API XmediaNativeDisplay* xmedia_display_from_egl(void* egl_surface);

// Windows HWND. Kept explicit rather than folded into the Wrap() escape hatch
// because it is the one kind every Windows host asks for.
// Side effects: allocates a display handle.
XMEDIA_C_API XmediaNativeDisplay* xmedia_display_from_hwnd(void* hwnd);

// Android ANativeWindow / Surface.
// Side effects: allocates a display handle.
XMEDIA_C_API XmediaNativeDisplay*
xmedia_display_from_android_native_window(void* window);
// Side effects: allocates a display handle.
XMEDIA_C_API XmediaNativeDisplay*
xmedia_display_from_android_surface(void* surface);

// iOS: a CAMetalLayer or CAEAGLLayer.
// Side effects: allocates a display handle.
XMEDIA_C_API XmediaNativeDisplay*
xmedia_display_from_cametal_layer(void* layer);
// Side effects: allocates a display handle.
XMEDIA_C_API XmediaNativeDisplay* xmedia_display_from_caeagl_layer(void* layer);

// The generic door: |kind| is an XMEDIA_DISPLAY_* value and |release| is an
// optional callback run on the render sequence when the last reference dies
// (may be null). This is how a host with its own window abstraction plugs in
// without asking this library for a new entry point per toolkit.
//
// |release| receives BOTH |raw| (the pointer that was wrapped) and |user_data|
// (whatever the host passed). Passing user_data separately is the whole point:
// a C host that needs cleanup state has nowhere else to keep it, and deriving
// it from |raw| would be a guess. It runs on the render sequence, so it must
// not block.
// Side effects: allocates a display handle; |release| runs when the last
// reference dies.
XMEDIA_C_API XmediaNativeDisplay*
xmedia_display_wrap(void* raw, int kind,
                    void (*release)(void* raw, void* user_data),
                    void* user_data);

// A valid "render nowhere" target; the sink discards frames but audio keeps
// playing.
// Side effects: allocates a display handle.
XMEDIA_C_API XmediaNativeDisplay* xmedia_display_none(void);

// Drops one reference; null is a no-op.
// Side effects: frees the handle when the last reference dies.
XMEDIA_C_API void xmedia_display_release(XmediaNativeDisplay* display);
// The XMEDIA_DISPLAY_KIND_* this handle was created with.
// Side effects: none.
XMEDIA_C_API int xmedia_display_kind(const XmediaNativeDisplay* display);

// 0 when the display can render, non-zero when it is None() or failed to be
// created. Never use the pointer itself for this: a null pointer here means
// "could not create", and None() is a VALID non-rendering target -- the two
// are different states and conflating them is what makes "the PiP is not
// showing" undiagnosable (see the note on Player::scene()).
// Side effects: none.
XMEDIA_C_API int xmedia_display_is_valid(const XmediaNativeDisplay* display);

// Copy-into-your-buffer contract, same as the title: returns the full length
// excluding the NUL, and a value >= |capacity| means truncated.
// Side effects: none.
XMEDIA_C_API int32_t xmedia_display_debug_string(
    const XmediaNativeDisplay* display, char* out, int32_t capacity);

// Mirrors media::NativeDisplayKind. Frozen: append only.
#define XMEDIA_DISPLAY_KIND_NONE 0
#define XMEDIA_DISPLAY_KIND_X11_WINDOW 1
#define XMEDIA_DISPLAY_KIND_WAYLAND_SURFACE 2
#define XMEDIA_DISPLAY_KIND_SDL2_WINDOW 3
#define XMEDIA_DISPLAY_KIND_DRM_MASTER 4
#define XMEDIA_DISPLAY_KIND_GBM_DEVICE 5
#define XMEDIA_DISPLAY_KIND_ANDROID_NATIVE_WINDOW 6
#define XMEDIA_DISPLAY_KIND_ANDROID_SURFACE 7
#define XMEDIA_DISPLAY_KIND_CAMETAL_LAYER 8
#define XMEDIA_DISPLAY_KIND_CAEAGL_LAYER 9
#define XMEDIA_DISPLAY_KIND_HWND 10
#define XMEDIA_DISPLAY_KIND_GLFW_WINDOW 11
#define XMEDIA_DISPLAY_KIND_EGL_WINDOW 12

// Mirrors media::DemuxerStreamType. Frozen: append only.
#define XMEDIA_STREAM_UNKNOWN 0
#define XMEDIA_STREAM_AUDIO 1
#define XMEDIA_STREAM_VIDEO 2
#define XMEDIA_STREAM_TEXT 3

// Mirrors PlayerState. Frozen: append only.
#define XMEDIA_STATE_IDLE 0
#define XMEDIA_STATE_INITIALIZED 1
#define XMEDIA_STATE_PREPARING 2
#define XMEDIA_STATE_PREPARED 3
#define XMEDIA_STATE_STARTED 4
#define XMEDIA_STATE_PAUSED 5
#define XMEDIA_STATE_BUFFERING 6
#define XMEDIA_STATE_STOPPING 7
#define XMEDIA_STATE_STOPPED 8
#define XMEDIA_STATE_COMPLETED 9
#define XMEDIA_STATE_ERROR 10
#define XMEDIA_STATE_END 11

// ---------------------------------------------------------------------------
// Events -- the legacy (what, arg1, arg2, obj) quadruple
// ---------------------------------------------------------------------------
//
// This is the whole reason the C ABI exists (docs/05): an ijkplayer's Java
// mEventHandler is already written against these codes, so forwarding them
// unchanged is what makes the migration a drop-in. The numbers below are the
// ones ToLegacyEvent() in player/player_event.cc emits; they are an ABI
// contract, so they are duplicated here rather than referenced.
//
//   0     kFlushed
//   100   kError          arg1 = ErrorCode, obj = message
//   200   kPrepared
//   300   kCompleted
//   400   kVideoSizeChanged arg1 = width, arg2 = height
//   402/403 kRenderingStarted (video / audio)
//   404   kRotationChanged arg1 = degrees
//   405/406 kDecodingStarted (audio / video)
//   407/408/409 kStageReached (open-input / find-stream-info / other)
//   500/501 kBufferingStarted / kBufferingEnded
//   502   kBufferingProgress arg1 = buffered ahead ms, arg2 = percent
//   600   kSeekCompleted  arg1 = actual ms, arg2 = Status
//   700   kStateChanged
//   800   kTimedText      obj = the cue text
//   900   kAccurateSeekCompleted arg1 = position ms
//   1000  kSnapshotCompleted arg1 = at ms, arg2 = Status, obj = file path
//   10001 kDecoderOpened
//
// Events with no ijkplayer equivalent (kDecoderFallback, kWaiting,
// kBufferedBytesUpdate, kBufferedTimeUpdate, kSeekRenderingStarted,
// kTrackChanged, kStats, kHdrUnsupported) are reported with what == 0, and
// their detail is available in full JSON via xmedia_player_dump_diagnostics().
// Dropping them silently is the alternative; what == 0 makes "this host does
// not know that event" observable instead of invisible.

// |obj| is owned by the library and valid only for the duration of the call.
typedef void (*XmediaEventCallback)(void* user_data, int what, int64_t arg1,
                                    int64_t arg2, const char* obj);

// Replaces any previous callback (pass null to remove it). The callback runs
// on the event sequence, never on a pipeline sequence, and it is safe to call
// any xmedia_player_* function from inside it -- including stop and reset.
// That is the deadlock fix over ijkplayer's message_loop; do not reintroduce
// a "must not reenter" rule here.
// Side effects: starts event delivery to |cb|.
XMEDIA_C_API void xmedia_player_set_event_callback(XmediaPlayer* player,
                                                   XmediaEventCallback cb,
                                                   void* user_data);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // XMEDIA_PLAYER_PUBLIC_C_XMEDIA_C_H_

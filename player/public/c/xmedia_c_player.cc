// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
// C ABI: player surface -- docs/08 M15.
//
// Lifecycle, transport control, queries, media info and multi-source handles.
// Every function forwards to xmedia::Player; see xmedia_c.cc's header comment
// for why there is deliberately no C-side logic here.
//
// NO EXCEPTIONS: the tree builds with -fno-exceptions, so nothing can unwind
// through a C frame and these entry points need no unwinding guard. Failures
// arrive as Status and are converted to XmediaStatus by ToC().

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "player/public/c/xmedia_c_internal.h"

// Version / global
// ---------------------------------------------------------------------------

extern "C" const char* xmedia_c_version(void) {
  // Bumped by hand and only on a breaking change to this header. A host built
  // against an older header keeps working against a newer library, because new
  // symbols are purely additive.
  return "1.0";
}

extern "C" const char* xmedia_engine_version(void) {
  return xmedia::GetVersion();
}

extern "C" void xmedia_global_init(void) {
  xmedia::GlobalInit();
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

extern "C" const char* xmedia_status_name(XmediaStatus status) {
  switch (status) {
  case XMEDIA_OK:
    return "kOk";
  case XMEDIA_ERR_INVALID_ARGUMENT:
    return "kInvalidArgument";
  case XMEDIA_ERR_INVALID_STATE:
    return "kInvalidState";
  case XMEDIA_ERR_NOT_IMPLEMENTED:
    return "kNotImplemented";
  case XMEDIA_ERR_OUT_OF_MEMORY:
    return "kOutOfMemory";
  case XMEDIA_ERR_ABORTED:
    return "kAborted";
  case XMEDIA_ERR_TIMEOUT:
    return "kTimeout";
  case XMEDIA_ERR_SOURCE_OPEN_FAILED:
    return "kSourceOpenFailed";
  case XMEDIA_ERR_SOURCE_NOT_FOUND:
    return "kSourceNotFound";
  case XMEDIA_ERR_SOURCE_READ_FAILED:
    return "kSourceReadFailed";
  case XMEDIA_ERR_DECODER_NOT_FOUND:
    return "kDecoderNotFound";
  case XMEDIA_ERR_DECODER_OPEN_FAILED:
    return "kDecoderOpenFailed";
  case XMEDIA_ERR_DECODE_FAILED:
    return "kDecodeFailed";
  case XMEDIA_ERR_DECODER_UNSUPPORTED_CODEC:
    return "kDecoderUnsupportedCodec";
  case XMEDIA_ERR_SINK_NOT_ATTACHED:
    return "kSinkNotAttached";
  case XMEDIA_ERR_STREAM_NOT_FOUND:
    return "kStreamNotFound";
  case XMEDIA_ERR_TRACK_NOT_FOUND:
    return "kTrackNotFound";
  case XMEDIA_ERR_CONFIG_INVALID:
    return "kConfigInvalid";
  case XMEDIA_ERR_EOS:
    return "kEos";
  default:
    return "kUnknown";
  }
}

extern "C" const char* xmedia_status_message(XmediaStatus status) {
  // The engine owns the sentences, so the C ABI never grows a second copy that
  // could drift from the C++ one. An unnamed code yields "" as promised.
  const char* name =
      xmedia::GetErrorCodeName(static_cast<xmedia::ErrorCode>(status));
  return name != nullptr ? name : "";
}

// ---------------------------------------------------------------------------
// Player lifecycle
// ---------------------------------------------------------------------------

extern "C" XmediaPlayer* xmedia_player_create(void) {
  xmedia::GlobalInit();
  // The nothrow forms are what make the documented "null means out of memory"
  // contract actually hold: under -fno-exceptions a plain new would terminate
  // the process instead of returning null.
  std::unique_ptr<XmediaPlayer> handle(new (std::nothrow) XmediaPlayer());
  if (handle == nullptr) {
    return nullptr;
  }
  handle->player =
      std::unique_ptr<xmedia::Player>(new (std::nothrow) xmedia::Player());
  if (handle->player == nullptr) {
    return nullptr;
  }
  return handle.release();
}

extern "C" void xmedia_player_destroy(XmediaPlayer* player) {
  // Destroying null is a no-op, so a cleanup path can call it unconditionally
  // after a failed create.
  delete player;
}

extern "C" void xmedia_player_stop_sync(XmediaPlayer* player,
                                        int64_t timeout_ms) {
  if (player == nullptr || player->player == nullptr) {
    return;
  }
  player->player->StopSync(
      xmedia::base::Milliseconds(std::max<int64_t>(0, timeout_ms)));
}

extern "C" XmediaStatus xmedia_player_set_data_source(XmediaPlayer* player,
                                                      const char* uri) {
  if (player == nullptr || player->player == nullptr || uri == nullptr) {
    return XMEDIA_ERR_INVALID_ARGUMENT;
  }
  return ToC(player->player->SetDataSource(std::string_view(uri)));
}

extern "C" XmediaStatus xmedia_player_prepare(XmediaPlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return XMEDIA_ERR_INVALID_ARGUMENT;
  }
  return ToC(player->player->PrepareAsync());
}

extern "C" XmediaStatus xmedia_player_prepare_sync(XmediaPlayer* player,
                                                   int64_t timeout_ms) {
  if (player == nullptr || player->player == nullptr) {
    return XMEDIA_ERR_INVALID_ARGUMENT;
  }
  return ToC(player->player->PrepareSync(
      xmedia::base::Milliseconds(std::max<int64_t>(0, timeout_ms))));
}

// ---------------------------------------------------------------------------
// Player transport
// ---------------------------------------------------------------------------

extern "C" void xmedia_player_start(XmediaPlayer* player) {
  if (player != nullptr && player->player != nullptr) {
    player->player->Start();
  }
}

extern "C" void xmedia_player_pause(XmediaPlayer* player) {
  if (player != nullptr && player->player != nullptr) {
    player->player->Pause();
  }
}

extern "C" void xmedia_player_stop(XmediaPlayer* player) {
  if (player != nullptr && player->player != nullptr) {
    player->player->Stop();
  }
}

extern "C" void xmedia_player_reset(XmediaPlayer* player) {
  if (player != nullptr && player->player != nullptr) {
    player->player->Reset();
  }
}

extern "C" void xmedia_player_step_once(XmediaPlayer* player) {
  if (player != nullptr && player->player != nullptr) {
    player->player->StepOnce();
  }
}

extern "C" void xmedia_player_seek_to(XmediaPlayer* player,
                                      int64_t position_ms) {
  if (player == nullptr || player->player == nullptr) {
    return;
  }
  // Fire and forget on purpose. The C++ SeekTo returns a request id so a host
  // can correlate rapid seeks, but that id also arrives in the event payload --
  // a host needing per-request correlation should use the C++ API.
  player->player->SeekTo(
      xmedia::base::Milliseconds(std::max<int64_t>(0, position_ms)));
}

extern "C" void xmedia_player_set_playback_rate(XmediaPlayer* player,
                                                double rate) {
  if (player != nullptr && player->player != nullptr) {
    player->player->SetPlaybackRate(rate);
  }
}

extern "C" void xmedia_player_set_volume(XmediaPlayer* player, double volume) {
  if (player != nullptr && player->player != nullptr) {
    player->player->SetVolume(volume);
  }
}

extern "C" void xmedia_player_set_muted(XmediaPlayer* player, int muted) {
  if (player != nullptr && player->player != nullptr) {
    player->player->SetMuted(muted != 0);
  }
}

extern "C" void xmedia_player_set_loop_count(XmediaPlayer* player, int count) {
  if (player != nullptr && player->player != nullptr) {
    player->player->SetLoopCount(count);
  }
}

extern "C" XmediaStatus xmedia_player_select_track(XmediaPlayer* player,
                                                   int stream_type,
                                                   int stream_index) {
  if (player == nullptr || player->player == nullptr) {
    return XMEDIA_ERR_INVALID_ARGUMENT;
  }
  const auto type = FromStreamType(stream_type);
  // kUnknown means the caller passed a code this ABI does not name. Saying so
  // beats letting it reach the demuxer and come back as a missing track: the
  // two
  // have completely different fixes.
  if (type == xmedia::media::DemuxerStreamType::kUnknown) {
    return XMEDIA_ERR_INVALID_ARGUMENT;
  }
  return ToC(player->player->SelectTrack(type, stream_index));
}

extern "C" void xmedia_player_set_video_surface(XmediaPlayer* player,
                                                XmediaNativeDisplay* display) {
  if (player == nullptr || player->player == nullptr) {
    return;
  }
  // A null display means "render nowhere but keep the audio" -- the documented
  // background-playback case, not an error.
  player->player->SetVideoSurface(display != nullptr ? display->display
                                                     : nullptr);
}

extern "C" XmediaStatus xmedia_player_take_snapshot(XmediaPlayer* player,
                                                    int64_t at_ms,
                                                    const char* file_path) {
  if (player == nullptr || player->player == nullptr || file_path == nullptr) {
    return XMEDIA_ERR_INVALID_ARGUMENT;
  }
  return ToC(player->player->TakeSnapshot(
      xmedia::base::Milliseconds(std::max<int64_t>(0, at_ms)),
      std::string(file_path)));
}

extern "C" void xmedia_player_reconnect_now(XmediaPlayer* player) {
  if (player != nullptr && player->player != nullptr) {
    player->player->ReconnectNow();
  }
}

// ---------------------------------------------------------------------------
// Player queries: lock-free snapshots, callable from any thread
// ---------------------------------------------------------------------------

extern "C" int xmedia_player_state(const XmediaPlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return XMEDIA_STATE_IDLE;
  }
  return static_cast<int>(player->player->state());
}

extern "C" int64_t xmedia_player_media_time_ms(const XmediaPlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return ToMs(player->player->GetMediaTime());
}

extern "C" int64_t xmedia_player_buffered_time_ms(const XmediaPlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return ToMs(player->player->GetBufferedTime());
}

extern "C" int64_t xmedia_player_duration_ms(const XmediaPlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return ToMs(player->player->GetDuration());
}

extern "C" int xmedia_player_is_live(const XmediaPlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return player->player->is_live() ? 1 : 0;
}

extern "C" int xmedia_player_is_playing(const XmediaPlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return player->player->IsPlaying() ? 1 : 0;
}

extern "C" int32_t xmedia_player_video_width(const XmediaPlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return player->player->video_natural_size().width;
}

extern "C" int32_t xmedia_player_video_height(const XmediaPlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return player->player->video_natural_size().height;
}

extern "C" int32_t xmedia_player_video_rotation(const XmediaPlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return player->player->video_rotation();
}

extern "C" double xmedia_player_playback_rate(const XmediaPlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 1.0;
  }
  return player->player->playback_rate();
}

extern "C" double xmedia_player_volume(const XmediaPlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0.0;
  }
  return player->player->volume();
}

extern "C" int xmedia_player_muted(const XmediaPlayer* player) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return player->player->muted() ? 1 : 0;
}

extern "C" int32_t xmedia_player_title(XmediaPlayer* player, char* out,
                                       int32_t capacity) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  // MediaInfo has no title field; the URI is what a window title would show
  // anyway, and synthesising one from the codec would be a lie.
  const auto info = player->player->media_info();
  return CopyOut(info ? info->uri : std::string(), out, capacity);
}

extern "C" int32_t xmedia_player_dump_diagnostics(const XmediaPlayer* player,
                                                  char* out, int32_t capacity) {
  if (player == nullptr || player->player == nullptr) {
    return 0;
  }
  return CopyOut(player->player->DumpDiagnostics(), out, capacity);
}

// ---------------------------------------------------------------------------
// Media info
// ---------------------------------------------------------------------------

extern "C" XmediaStatus xmedia_player_get_media_info(const XmediaPlayer* player,
                                                     XmediaMediaInfo* out) {
  if (player == nullptr || player->player == nullptr || out == nullptr) {
    return XMEDIA_ERR_INVALID_ARGUMENT;
  }
  const auto info = player->player->media_info();
  if (!info) {
    // Before prepare there is genuinely no answer, and zeros with XMEDIA_OK
    // would be the classic lie.
    return XMEDIA_ERR_INVALID_STATE;
  }
  out->duration_ms = ToMs(info->duration);
  out->bitrate = info->bit_rate;
  out->is_live = info->is_live ? 1 : 0;
  out->seekable = info->seekable ? 1 : 0;
  out->video_width = 0;
  out->video_height = 0;
  out->video_rotation = 0;
  out->video_track_count = 0;
  out->audio_track_count = 0;
  out->subtitle_track_count = 0;
  // Size and rotation live on the stream, not the container. The FIRST video
  // stream wins, which is the same rule MediaInfo::FirstStreamOfKind applies.
  for (const auto& stream : info->streams) {
    switch (stream.kind) {
    case xmedia::StreamKind::kVideo:
      ++out->video_track_count;
      if (out->video_width == 0 && out->video_height == 0) {
        out->video_width = stream.natural_size.width;
        out->video_height = stream.natural_size.height;
        out->video_rotation = stream.rotation;
      }
      break;
    case xmedia::StreamKind::kAudio:
      ++out->audio_track_count;
      break;
    case xmedia::StreamKind::kText:
      ++out->subtitle_track_count;
      break;
    case xmedia::StreamKind::kUnknown:
      break;
    }
  }
  return XMEDIA_OK;
}

// ---------------------------------------------------------------------------
// Multi-source composition
// ---------------------------------------------------------------------------

extern "C" XmediaSource* xmedia_player_add_source(XmediaPlayer* player,
                                                  const char* uri,
                                                  const char* name,
                                                  int32_t z_order) {
  if (player == nullptr || player->player == nullptr || uri == nullptr) {
    return nullptr;
  }
  xmedia::DataSourceDescriptor descriptor;
  descriptor.uri = std::string(uri);
  auto source = player->player->AddSource(descriptor,
                                          name != nullptr ? name : "", z_order);
  if (source == nullptr) {
    return nullptr;
  }
  std::unique_ptr<XmediaSource> handle(new (std::nothrow) XmediaSource());
  if (handle == nullptr) {
    return nullptr;
  }
  handle->source = std::move(source);
  return handle.release();
}

extern "C" void xmedia_source_release(XmediaSource* source) {
  // Null is a no-op. This only drops this reference, matching the C++
  // shared_ptr semantics -- it does not unregister the source.
  delete source;
}

extern "C" XmediaStatus xmedia_source_set_uri(XmediaSource* source,
                                              const char* uri) {
  if (source == nullptr || source->source == nullptr || uri == nullptr) {
    return XMEDIA_ERR_INVALID_ARGUMENT;
  }
  // SceneSource has no mutating URI in the C++ API today. NOT_IMPLEMENTED is
  // the honest answer; inventing one here would create behaviour with no C++
  // counterpart, and a divergence that only shows up on the C side.
  return XMEDIA_ERR_NOT_IMPLEMENTED;
}

// ---------------------------------------------------------------------------

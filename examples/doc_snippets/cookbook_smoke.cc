// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//
// COOKBOOK compile smoke: every snippet docs/COOKBOOK.md ships must compile
// (docs/10 §12, V8: "COOKBOOK.md has >= 20 entries, each compilable"). This
// file is the executable form of that claim for the entries whose code the
// full examples do not already cover -- the ones play_sdl2/pull_frames
// exercise (window, event loop, custom sink) are proven by those binaries.
//
// Each block below is copied verbatim from the cookbook entry it names. If a
// signature drifts, THIS is the file that goes red, not a user's build.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/scoped_refptr.h"
#include "base/time/time.h"
#include "media/base/data_source_descriptor.h"
#include "media/base/media_info.h"
#include "player/public/deps.h"
#include "player/public/playback_stats.h"
#include "player/public/player.h"
#include "player/public/player_config.h"

namespace {

using xmedia::Player;
using xmedia::PlayerConfig;
using xmedia::PlayerEvent;
using xmedia::PlayerState;
using xmedia::Status;

// ---- entry 20: route logs into the host's own system --------------------
class MyLog final : public xmedia::base::logging::LoggingDelegate {
 public:
  void OnLogMessage(xmedia::base::logging::LogSeverity severity,
                    const char* file, int line,
                    std::string_view message) override {
    std::fprintf(stderr, "[host %d %s:%d] %.*s\n", static_cast<int>(severity),
                 file, line, static_cast<int>(message.size()), message.data());
  }
};

// ---- entry 4: a custom data source (interface shape only) ----------------
class MySource : public xmedia::media::DataSource {
 public:
  void SetHost(Host* host) override { host_ = host; }
  void Read(int64_t offset, size_t size, uint8_t* data,
            xmedia::base::scoped_refptr<xmedia::base::TaskRunner> task_runner,
            ReadCB read_cb) override {
    (void)offset;
    (void)size;
    (void)data;
    task_runner->PostTask(
        FROM_HERE,
        xmedia::base::BindOnce(std::move(read_cb),
                               xmedia::media::DataSource::ReadResult(0)));
  }
  xmedia::media::DataSource::ReadResult
  ReadBlocking(int64_t offset, size_t size, uint8_t* data) override {
    (void)offset;
    (void)size;
    (void)data;
    return 0;  // EOF
  }
  void Abort() override {}
  bool GetSize(int64_t* size_out) override {
    *size_out = 0;
    return true;
  }
  bool IsStreaming() const override { return false; }
  void SetBitrate(int bitrate) override { bitrate_ = bitrate; }
  bool IsSeekable() const override { return true; }

 private:
  Host* host_{nullptr};
  int bitrate_{0};
};

void CookbookEntries() {
  const std::string url = "file.mp4";
  const std::vector<uint8_t> bytes;

  // ---- entry 2: network stream tuning ---------------------------------
  PlayerConfig config;
  config.net.user_agent = "my-host/1.0";
  config.net.headers = {{"Referer", "https://example.com/"}};
  config.net.reconnect = true;
  config.net.reconnect_max_retries = 5;
  config.net.reconnect_delay = xmedia::base::Milliseconds(200);
  config.demux.analyze_duration = xmedia::base::Milliseconds(100);

  // ---- entry 17: weak-network tuning ----------------------------------
  config.buffer.enabled = true;
  config.buffer.max_bytes = 30 * 1024 * 1024;
  config.buffer.first_high_water_mark = xmedia::base::Milliseconds(100);
  config.buffer.next_high_water_mark = xmedia::base::Seconds(1);
  config.buffer.last_high_water_mark = xmedia::base::Seconds(5);
  config.buffer.unlimited = false;

  // ---- entry 18: chase the live edge ----------------------------------
  config.net.live_max_latency = xmedia::base::Seconds(3);

  // ---- entry 19: force software / hardware decode ----------------------
  config.video.decoder_preference = xmedia::DecoderPreference::kSoftware;
  config.video.hw_codecs =
      static_cast<xmedia::HwCodecMask>(xmedia::HwCodecFlag::kHevc);

  // ---- entry 7: audio only --------------------------------------------
  config.render.disable_video_output = true;

  // ---- entry 11: tempo-preserving rate change --------------------------
  config.audio.tempo_stretch = true;
  config.audio.preserves_pitch = true;

  auto deps = std::make_unique<xmedia::Deps>();
  deps->logging_delegate = std::make_unique<MyLog>();            // entry 20
  deps->data_source = xmedia::base::MakeRefCounted<MySource>();  // entry 4

  // ---- entry 22: ijk option keys (PlayerBuilder, not Player) --------------
  xmedia::PlayerBuilder builder;
  builder.SetConfig(config);
  builder.SetOption(xmedia::OptionCategory::kFormat, "analyzeduration",
                    int64_t{500});
  builder.SetOption(xmedia::OptionCategory::kPlayer, "max-buffer-size",
                    int64_t{30 * 1024 * 1024});
  builder.SetOption(xmedia::OptionCategory::kPlayer, "nodisp", true);
  auto built = builder.Build();
  if (!built) {
    return;
  }
  std::unique_ptr<Player> player = std::move(*built);

  // ---- generic skeleton: event handler + three-part errors -------------
  player->SetEventHandler(xmedia::base::BindRepeating([](const PlayerEvent& e) {
    switch (e.type) {
    case xmedia::EventType::kError: {  // entry 13 family
      if (const auto* p = xmedia::AsError(e)) {
        std::fprintf(stderr, "error: %s\n", p->error.ToString().c_str());
      }
      break;
    }
    case xmedia::EventType::kBufferingStarted:
    case xmedia::EventType::kBufferingEnded:
    case xmedia::EventType::kBufferingProgress:
    case xmedia::EventType::kWaiting:
    case xmedia::EventType::kDecoderFallback:
    case xmedia::EventType::kTrackChanged:
    case xmedia::EventType::kSnapshotCompleted:
    case xmedia::EventType::kAccurateSeekCompleted:
      break;
    default:
      break;
    }
  }));

  // ---- entry 1: local file --------------------------------------------
  if (const Status s = player->SetDataSource(url); !s) {
    return;
  }
  // ---- entry 3: from memory -------------------------------------------
  if (const Status s = player->SetDataSource(
          xmedia::DataSourceDescriptor::FromMemory(bytes.data(), bytes.size()));
      !s) {
    return;
  }

  // ---- entry 12: enumerate and select tracks ---------------------------
  if (const auto info = player->media_info()) {
    for (const auto& s : info->streams) {
      if (s.kind == xmedia::media::StreamKind::kAudio && s.index >= 0) {
        std::fprintf(stderr, "audio track %d: %s (%s)\n", s.index,
                     s.codec_name.c_str(), s.language.c_str());
      }
    }
    player->SelectTrack(xmedia::media::DemuxerStreamType::kAudio, 1);
  }

  // ---- entry 9: accurate seek ------------------------------------------
  // The callback carries Status only; the Result<int64_t> the call returns is
  // the REQUEST id (match it against SeekCompletedPayload), not a frame
  // number. Where the seek actually landed is reported by requested/actual.
  const xmedia::Result<int64_t> request_id =
      player->SeekTo(xmedia::base::Seconds(90), xmedia::SeekMode::kAccurate,
                     xmedia::base::BindOnce([](xmedia::Status s) {
                       if (!s) {
                         std::fprintf(stderr, "seek failed: %s\n",
                                      s.error().ToString().c_str());
                       }
                     }));
  if (request_id) {
    (void)*request_id;
  }

  // ---- entry 10/11/16: step, rate, loop --------------------------------
  player->Pause();
  player->StepOnce();
  player->SetPlaybackRate(2.0);
  player->SetLoopCount(-1);

  // ---- entry 8: snapshot -------------------------------------------------
  if (const Status s =
          player->TakeSnapshot(xmedia::base::Seconds(3), "/tmp/frame.png");
      !s) {
    return;
  }

  // ---- entry 14/15: stats and glitches -----------------------------------
  const xmedia::PlaybackStats st = player->GetPlaybackStats();
  xmedia::base::TimeDelta total = st.stages.total_first_frame;
  xmedia::base::TimeDelta open = st.stages.open_input_to_stream_info;
  xmedia::base::TimeDelta lat = st.first_frame_latency;
  const xmedia::media::AudioGlitchInfo& g = st.audio_glitches;
  std::fprintf(stderr, "first frame %s (%s + %s), glitches=%llu xruns=%llu\n",
               total.ToString().c_str(), open.ToString().c_str(),
               lat.ToString().c_str(),
               static_cast<unsigned long long>(g.total_glitches),
               static_cast<unsigned long long>(g.xruns));
  std::fprintf(stderr, "stats json: %s\n", st.ToJson().c_str());

  // ---- entry 21: diagnostics --------------------------------------------
  const std::string dump = player->DumpDiagnostics();
  std::fprintf(stderr, "diagnostics: %zu bytes\n", dump.size());

  // ---- teardown: StopSync, not Stop ---------------------------------------
  player->StopSync(xmedia::base::Seconds(5));
}

}  // namespace

int main() {
  CookbookEntries();
  std::printf("cookbook snippets compiled and linked\n");
  return 0;
}

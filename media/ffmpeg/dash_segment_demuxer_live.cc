// Copyright 2026 The xmedia-kit Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// The live half of DashSegmentDemuxer: re-fetching a dynamic MPD on its
// minimumUpdatePeriod and EXTENDING the chains in place. Split from
// dash_segment_demuxer.cc for the C1 line limit; the seam is "what a manifest
// means once" (open) versus "what it means again later" (reload).
//
// WHY EXTEND RATHER THAN REBUILD. A live window slides: old segments drop out
// of the manifest and new ones appear. The demuxer is already playing at some
// cursor inside its chain, so replacing media_urls would renumber everything
// under the cursor and make the timeline jump. Extending from the LAST known
// URL keeps every index that was already handed out valid; segments that fell
// out of the window simply stay in the list (they can still be served from the
// chain's already-opened segment or fail as 404, which is the honest answer).
//
// The reload also looks representations up by ID rather than by "highest
// bandwidth": a live manifest's bandwidths move, and re-running the selection
// rule would silently switch the rung mid-play. That is exactly the ABR
// decision this demuxer does not make.

#include <algorithm>
#include <cstddef>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/task/sequenced_task_runner.h"
#include "media/ffmpeg/dash_segment_demuxer.h"

namespace xmedia::media {
namespace {

// A reload this frequent would hammer the origin; one this slow would let a
// short window drain between refreshes. Real manifests advertise 2-10 s.
constexpr int64_t kMinReloadMs = 500;
constexpr int64_t kMaxReloadMs = 30'000;
constexpr int64_t kFallbackReloadMs = 5'000;

base::TimeDelta ReloadInterval(int64_t minimum_update_period_ms) {
  const int64_t ms =
      std::clamp(minimum_update_period_ms == 0 ? kFallbackReloadMs
                                               : minimum_update_period_ms,
                 kMinReloadMs, kMaxReloadMs);
  return base::Milliseconds(ms);
}

}  // namespace

void DashSegmentDemuxer::ScheduleMpdReload() {
  if (stopped_.load() || !mpd_.is_live || reload_scheduled_ ||
      media_task_runner_ == nullptr) {
    return;
  }
  reload_scheduled_ = true;
  media_task_runner_->PostDelayedTask(
      FROM_HERE,
      base::BindOnce(
          [](DashSegmentDemuxer* self) {
            self->reload_scheduled_ = false;
            self->FetchMpdReload();
          },
          base::Unretained(this)),
      ReloadInterval(mpd_.minimum_update_period_ms));
}

size_t DashSegmentDemuxer::ExtendChain(Chain* chain,
                                       const dash::MpdRepresentation& fresh) {
  if (chain == nullptr || fresh.segment_urls.empty()) {
    return 0;
  }
  // The init segment is fetched once per chain (at last open); a reload has no
  // reason to re-derive it, so skip the leading entry the parser prepends.
  const size_t first_media =
      chain->init_pending_url.empty() && chain->init_bytes.empty() ? 0 : 1;
  if (fresh.segment_urls.size() <= first_media) {
    return 0;
  }
  // Align on the LAST URL already known: everything after it is new. Matching
  // by URL rather than by count survives a window that dropped segments at the
  // front, which is what a live manifest does.
  size_t added = 0;
  if (chain->media_urls.empty()) {
    chain->media_urls.assign(fresh.segment_urls.begin() +
                                 static_cast<std::ptrdiff_t>(first_media),
                             fresh.segment_urls.end());
    added = chain->media_urls.size();
  } else {
    const std::string& tail = chain->media_urls.back();
    auto it = std::find(fresh.segment_urls.begin() +
                            static_cast<std::ptrdiff_t>(first_media),
                        fresh.segment_urls.end(), tail);
    if (it == fresh.segment_urls.end()) {
      // The window moved past what we last saw (a long stall, or the origin
      // renumbered). Appending nothing is safer than appending a gap: the
      // chain keeps its own tail and ends naturally.
      return 0;
    }
    for (auto next = it + 1; next != fresh.segment_urls.end(); ++next) {
      chain->media_urls.push_back(*next);
      ++added;
    }
  }
  if (added > 0) {
    chain->ended = false;  // new material un-ends the chain
  }
  return added;
}

void DashSegmentDemuxer::OnMpdReloaded(bool ok, std::string body) {
  if (stopped_.load()) {
    return;
  }
  if (!ok) {
    // A failed refresh is not a playback error: the buffered segments still
    // play, and the next tick tries again. Logged at most once per streak so a
    // dead origin does not flood the media log.
    if (++reload_failures_ == 1) {
      LOG(WARNING) << "xmedia.dash: live manifest reload failed (url = "
                   << mpd_url_ << "); keeping the segments already known";
    }
    ScheduleMpdReload();
    return;
  }
  auto parsed = dash::ParseMpd(body, mpd_url_);
  if (!parsed.has_value() || parsed->representations.empty()) {
    if (++reload_failures_ == 1) {
      LOG(WARNING) << "xmedia.dash: live manifest reload was unusable; "
                   << "keeping the segments already known";
    }
    ScheduleMpdReload();
    return;
  }
  reload_failures_ = 0;
  // Keep the OLD anchors: they define the numbering the chains are already
  // indexed against. A manifest that moves availabilityStartTime is starting a
  // new presentation, which is not something a reload can absorb.
  parsed->availability_start_time_ms = mpd_.availability_start_time_ms;
  mpd_.publish_time_ms = parsed->publish_time_ms;
  mpd_.minimum_update_period_ms = parsed->minimum_update_period_ms;
  mpd_.time_shift_buffer_depth_ms = parsed->time_shift_buffer_depth_ms;

  size_t added = 0;
  for (size_t i = 0; i < chains_.size(); ++i) {
    const std::string& rep_id =
        i < chain_rep_ids_.size() ? chain_rep_ids_[i] : std::string();
    const dash::MpdRepresentation* match = nullptr;
    for (const auto& rep : parsed->representations) {
      if (rep.id == rep_id) {
        match = &rep;
        break;
      }
    }
    if (match == nullptr) {
      continue;  // that representation left the manifest: nothing to extend
    }
    added += ExtendChain(&chains_[i], *match);
  }
  if (added > 0) {
    // A chain parked at its old tail now has more to play. Those reads are
    // still in pending_reads_ (the live path parks rather than answering EOS),
    // so re-issuing them is all "resume" means -- and a chain that had already
    // ended under the static rule is un-ended the same way.
    for (size_t i = 0; i < chains_.size(); ++i) {
      Chain& chain = chains_[i];
      chain.ended = false;
      if (chain.open_in_flight) {
        continue;  // an open is already in flight; its settle re-issues reads
      }
      if (chain.cursor + 1 >= chain.media_urls.size()) {
        continue;  // this refresh added nothing for this chain
      }
      // Re-issue whatever this chain parked at its old tail FIRST, so the
      // reads that were waiting see the new segments. AdvanceChain alone only
      // opens; the pending read is what turns the open into progress.
      const size_t chain_index = i;
      std::vector<PendingRead> mine;
      for (auto it = pending_reads_.begin(); it != pending_reads_.end();) {
        if (chain_index_for_type(it->type) == chain_index) {
          mine.push_back(std::move(*it));
          it = pending_reads_.erase(it);
          continue;
        }
        ++it;
      }
      AdvanceChain(chain_index);
      for (PendingRead& read : mine) {
        // The open is now in flight; park them again so the settle re-issues
        // them against the segment that is opening.
        pending_reads_.push_back(std::move(read));
      }
    }
  }
  ScheduleMpdReload();
}

}  // namespace xmedia::media

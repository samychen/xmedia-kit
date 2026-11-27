import { selectScene } from "../panels/scenes.js";
import { seekTo, updatePlayhead } from "../preview/driver.js";
import { startPhAnim } from "../preview/playhead.js";
import { log, $ } from "../shared/dom.js";
import { HEAD_W } from "../shared/state.js";
import { fmtTime, fps } from "../shared/time.js";
import {
  TRACKS, capSegments, capTrackSub, hashSeed, loadThumbsForClip,
  loadWaveForClip, mk, sceneTexts, snapDetail, snapRangeDetail,
  snapTime, stripHTML, waveSVG
} from "./model.js";
import { applyRangeGeom, ensureRangeOverlay, startRangeDrag, syncRangeUI } from "./range.js";
import { moveClipToTrack, reorderTo, splitScene, trimCommit, trimLeftCommit } from "./trim.js";

/* ---------------- 视觉辅助：磁吸线 / 浮动 HUD / 自动滚屏 / iframe 隔离 ---------------- */
function dragShield(on) {
  document.body.classList.toggle("tl-dragging", !!on);
}

function getSnapGuide() {
  var g = $("tl-grid");
  if (!g) return null;
  var el = $("tl-snap-guide");
  if (!el || el.parentNode !== g) {
    el = mk("div", "");
    el.id = "tl-snap-guide";
    g.appendChild(el);
  }
  return el;
}

function showSnapGuide(timeSec, kind) {
  var el = getSnapGuide();
  if (!el) return;
  if (timeSec == null || !isFinite(timeSec)) {
    el.style.display = "none";
    el.classList.remove("playhead");
    return;
  }
  el.style.display = "block";
  el.style.left = (HEAD_W + Math.max(0, timeSec) * tlPPS) + "px";
  el.classList.toggle("playhead", kind === "playhead");
}

function showDragHud(clientX, clientY, text) {
  var hud = $("tl-drag-hud");
  if (!hud) {
    hud = mk("div", "");
    hud.id = "tl-drag-hud";
    document.body.appendChild(hud);
  }
  if (!text) { hud.style.display = "none"; return; }
  hud.textContent = text;
  hud.style.display = "block";
  hud.style.left = Math.max(60, Math.min(window.innerWidth - 60, clientX)) + "px";
  hud.style.top = Math.max(28, clientY - 14) + "px";
}

function hideDragOverlays() {
  dragShield(false);
  showSnapGuide(null, null);
  showDragHud(0, 0, "");
  document.querySelectorAll(".tl-row.drop-target").forEach(function (r) {
    r.classList.remove("drop-target");
  });
}

function makeAutoScroller(onScrollTick, canExpandRight) {
  var raf = 0;
  var vx = 0;
  var lastEv = null;
  function step() {
    raf = 0;
    var sc = $("tl-scroll");
    if (!sc || !vx) return;
    if (vx > 0 && canExpandRight) {
      var gridEl = $("tl-grid");
      var needW = sc.scrollLeft + sc.clientWidth + vx + 40;
      if (gridEl && parseFloat(gridEl.style.width || "0") < needW) {
        gridEl.style.width = needW + "px";
      }
    }
    var next = Math.max(0, Math.min(sc.scrollWidth - sc.clientWidth, sc.scrollLeft + vx));
    if (next !== sc.scrollLeft) {
      sc.scrollLeft = next;
      if (onScrollTick && lastEv) onScrollTick(lastEv);
    }
    raf = requestAnimationFrame(step);
  }
  return {
    update: function (ev) {
      lastEv = ev;
      var sc = $("tl-scroll");
      if (!sc) return;
      var rect = sc.getBoundingClientRect();
      var leftEdge = rect.left + HEAD_W + 32;
      var rightEdge = rect.right - 32;
      if (ev.clientX < leftEdge && sc.scrollLeft > 0) {
        vx = -Math.max(4, Math.min(18, (leftEdge - ev.clientX) * 0.45));
      } else if (ev.clientX > rightEdge &&
                 (canExpandRight || sc.scrollLeft < sc.scrollWidth - sc.clientWidth)) {
        vx = Math.max(4, Math.min(18, (ev.clientX - rightEdge) * 0.45));
      } else {
        vx = 0;
      }
      if (vx && !raf) raf = requestAnimationFrame(step);
    },
    stop: function () {
      vx = 0;
      lastEv = null;
      if (raf) { cancelAnimationFrame(raf); raf = 0; }
    }
  };
}

/* 主轨（track 0 顺序流）拖拽插入位判定：仅统计顺序主轨片段，忽略叠加轨。 */
function dropInfoTrack0(t, dragIdx) {
  var specs = (state.spec && state.spec.scenes) || [];
  var sc = state.scenes || [];
  var seq = [];
  for (var i = 0; i < sc.length; i++) {
    var sp = specs[i] || {};
    if ((Number(sp.track) || 0) !== 0 || sp.start !== undefined) continue;
    seq.push({ idx: i, start: sc[i].start || 0, dur: sc[i].dur || 0 });
  }
  var ord = 0;
  for (var j = 0; j < seq.length; j++) {
    var item = seq[j];
    if (item.idx === dragIdx) continue;
    if (t < item.start + item.dur / 2) {
      return { target: item.idx, ord: ord + 1, lineX: item.start * tlPPS };
    }
    ord++;
  }
  var last = seq.length ? seq[seq.length - 1] : null;
  return {
    target: specs.length,
    ord: ord + 1,
    lineX: last ? (last.start + last.dur) * tlPPS : 0
  };
}

/* 单个场景 clip 构建（多轨各行共用）。 */
function buildSceneClip(idx, lane, tr) {
  var spec = (state.spec.scenes || [])[idx] || {};
  var locked = !!spec.narration;
  var s = state.scenes[idx] ||
      { start: 0, dur: Number(spec.duration) || 3,
        label: (idx + 1) + " · " + (spec.type || "text") };
  var total = Math.max(state.duration || (s.start + s.dur), 0.1);
  var w = Math.max(14, s.dur * tlPPS);
  var b = mk("div", "clip scene" + (locked ? " locked" : "") +
             (idx === selIdx ? " sel" : ""));
  b.dataset.idx = String(idx);
  b.dataset.seq = ((Number(spec.track) || 0) === 0 && spec.start === undefined) ? "1" : "0";
  b.style.left = (s.start * tlPPS) + "px";
  b.style.width = w + "px";
  b.title = (locked ? "旁白锁定：可拖动重排或跨轨 · 右键切割禁用"
                    : "点选 · 拖动重排/跨轨 · 拖左右缘裁剪 · 右键播放头切割") +
            " · " + s.label;
  b.innerHTML = '<div class="cap">' + (idx + 1) + " · " + s.label +
    '<span class="dur">' + s.dur.toFixed(2) + "s</span></div>" +
    stripHTML(s, idx, w);
  if (spec.video) {
    var _strip = b.querySelector(".strip");
    if (_strip) {
      loadThumbsForClip(_strip, spec.video, spec.video_from || 0, s.dur);
    }
  }

  b.addEventListener("dblclick", function (ev) {
    if (ev.target.closest(".trim")) return;
    selectScene(idx, false);
    zoomToScene(idx);
  });

  // 右键 = 在播放头处切开这一段
  b.addEventListener("contextmenu", function (ev) {
    ev.preventDefault();
    if (locked) {
      log("scene-" + (idx + 1) + " 旁白锁定，不支持切割", "err");
      return;
    }
    selectScene(idx, false);
    splitScene(idx, lastTime);
  });

  /* ---------- 1. 片段主体拖动（水平重排 / 自由平移 / 跨轨投放） ---------- */
  b.addEventListener("pointerdown", function (e) {
    if (e.button !== 0 || e.shiftKey || tlRangeMode) return;
    if (e.target.closest(".trim") || e.target.closest(".cmt-badge")) return;
    e.stopPropagation();

    var pid = e.pointerId;
    var px0 = e.clientX, py0 = e.clientY;
    var scEl = $("tl-scroll");
    var scroll0 = scEl ? scEl.scrollLeft : 0;
    var grabOffPx = e.clientX - b.getBoundingClientRect().left;
    var dragging = false;
    var cancelled = false;
    var dropLine = null;
    var ghost = null;
    var activeLane = null;
    var curTargetTrack = Number(tr);
    var curDropInfo = null;
    var curOverlayStart = s.start;

    if (b.setPointerCapture && pid !== undefined) {
      try { b.setPointerCapture(pid); } catch { /* ignore */ }
    }

    function clearVisualTargets() {
      if (dropLine) { dropLine.remove(); dropLine = null; }
      if (ghost) { ghost.remove(); ghost = null; }
      document.querySelectorAll(".tl-row.drop-target").forEach(function (r) {
        r.classList.remove("drop-target");
      });
    }

    function evaluatePointer(e2) {
      var dx = e2.clientX - px0;
      var dy = e2.clientY - py0;
      var dScroll = scEl ? (scEl.scrollLeft - scroll0) : 0;
      if (!dragging && Math.hypot(dx, dy) < 5) return;
      if (!dragging) {
        dragging = true;
        b.classList.add("dragging");
        dragShield(true);
      }
      b.style.transform = "translate3d(" + (dx + dScroll) + "px, " + dy + "px, 0)";

      var over = document.elementFromPoint(e2.clientX, e2.clientY);
      var lane2 = over && over.closest ? over.closest(".tl-lane[data-kind='scene']") : null;
      if (!lane2) lane2 = activeLane || lane;
      activeLane = lane2;
      var t2 = lane2 && lane2.dataset.track !== undefined ? Number(lane2.dataset.track) : Number(tr);
      curTargetTrack = t2;

      clearVisualTargets();
      if (lane2 && lane2.parentNode) lane2.parentNode.classList.add("drop-target");

      var laneRect = lane2.getBoundingClientRect();
      var cursorT = (e2.clientX - laneRect.left) / tlPPS;
      if (t2 === 0) {
        showSnapGuide(null, null);
        curDropInfo = dropInfoTrack0(cursorT, idx);
        dropLine = mk("div", "drop-line");
        dropLine.style.left = curDropInfo.lineX + "px";
        lane2.appendChild(dropLine);
        showDragHud(e2.clientX, laneRect.top,
          "主轨 · 移至第 " + curDropInfo.ord + " 位 (" + s.dur.toFixed(2) + "s)");
      } else {
        var rawStart = Math.max(0, (e2.clientX - laneRect.left - grabOffPx) / tlPPS);
        var res = e2.altKey
          ? { start: rawStart, snapTime: null, snapType: null }
          : snapRangeDetail(rawStart, s.dur, idx);
        curOverlayStart = res.start;
        ghost = mk("div", "clip-ghost");
        ghost.style.left = (res.start * tlPPS) + "px";
        ghost.style.width = Math.max(14, s.dur * tlPPS) + "px";
        lane2.appendChild(ghost);
        showSnapGuide(res.snapTime, res.snapType);
        showDragHud(e2.clientX, laneRect.top,
          "叠加轨 " + t2 + " · @" + res.start.toFixed(2) + "s" +
          (res.snapType === "playhead" ? " [吸附播放头]" : res.snapType === "edge" ? " [吸附边]" : ""));
      }
    }

    var scroller = makeAutoScroller(evaluatePointer);

    function onKey(kev) {
      if (kev.key === "Escape") {
        kev.preventDefault();
        cancelled = true;
        cleanup();
      }
    }

    function cleanup() {
      scroller.stop();
      window.removeEventListener("pointermove", mv);
      window.removeEventListener("pointerup", up);
      window.removeEventListener("pointercancel", cancel);
      window.removeEventListener("keydown", onKey, true);
      b.classList.remove("dragging");
      b.style.transform = "";
      clearVisualTargets();
      hideDragOverlays();
    }

    function mv(e2) {
      if (pid !== undefined && e2.pointerId !== undefined && e2.pointerId !== pid) return;
      evaluatePointer(e2);
      if (dragging) scroller.update(e2);
    }

    function cancel(e2) {
      if (pid !== undefined && e2.pointerId !== undefined && e2.pointerId !== pid) return;
      cancelled = true;
      cleanup();
    }

    function up(e2) {
      if (pid !== undefined && e2.pointerId !== undefined && e2.pointerId !== pid) return;
      var wasDragging = dragging;
      cleanup();
      if (cancelled) return;
      if (!wasDragging) {
        if (selIdx !== idx) selectScene(idx, false);
        return;
      }
      if (curTargetTrack === 0) {
        if (curDropInfo) reorderTo(idx, curDropInfo.target);
      } else {
        moveClipToTrack(idx, curTargetTrack, curOverlayStart);
      }
    }

    window.addEventListener("pointermove", mv);
    window.addEventListener("pointerup", up);
    window.addEventListener("pointercancel", cancel);
    window.addEventListener("keydown", onKey, true);
  });

  /* ---------- 2. 左右双缘裁剪（左缘调整起点/入点 · 右缘自由延长并波纹推开后续） ---------- */
  if (!locked) {
    var trimL = mk("div", "trim left");
    trimL.title = "拖左缘裁剪起点" + (spec.video ? "（同步顺延视频入点）" : "");
    var trimR = mk("div", "trim right");
    trimR.title = "拖右缘裁剪或延长时长（Alt 临时关闭吸附 · Esc 取消）";

    // 2a. 右缘裁剪：突破原 total - s.start 封顶，顺序主轨实时波纹平移后续片段
    trimR.addEventListener("pointerdown", function (e) {
      if (e.button !== 0) return;
      e.stopPropagation();
      e.preventDefault();

      var pid = e.pointerId;
      var px0 = e.clientX;
      var scEl = $("tl-scroll");
      var scroll0 = scEl ? scEl.scrollLeft : 0;
      var d0 = s.dur;
      var curDur = d0;
      var cancelled = false;
      var isSeq = ((Number(spec.track) || 0) === 0 && spec.start === undefined);
      var durLabel = b.querySelector(".dur");
      var gridEl = $("tl-grid");
      var rulerEl = gridEl ? gridEl.querySelector(".tl-ruler") : null;
      var laneEls = gridEl ? Array.from(gridEl.querySelectorAll(".tl-lane")) : [];
      var downstream = [];
      if (isSeq) {
        lane.querySelectorAll('.clip.scene[data-seq="1"]').forEach(function (el) {
          if (Number(el.dataset.idx) > idx) downstream.push(el);
        });
      }

      trimR.classList.add("active");
      dragShield(true);
      if (trimR.setPointerCapture && pid !== undefined) {
        try { trimR.setPointerCapture(pid); } catch { /* ignore */ }
      }

      function evalRight(e2) {
        var dScroll = scEl ? (scEl.scrollLeft - scroll0) : 0;
        var rawDur = Math.max(0.3, Math.min(300, d0 + (e2.clientX - px0 + dScroll) / tlPPS));
        var endCandidate = s.start + rawDur;
        var res = e2.altKey
          ? { time: endCandidate, snapped: false, type: null }
          : snapDetail(endCandidate, idx, s.start + 300);
        curDur = Math.max(0.3, res.time - s.start);
        var dd = curDur - d0;

        b.style.width = Math.max(14, curDur * tlPPS) + "px";
        if (durLabel) durLabel.textContent = curDur.toFixed(2) + "s";

        if (isSeq) {
          downstream.forEach(function (el) {
            el.style.transform = "translateX(" + (dd * tlPPS) + "px)";
          });
          var expTotal = Math.max(total, total + dd);
          var expCw = Math.max(220, Math.round(expTotal * tlPPS));
          if (gridEl) gridEl.style.width = (HEAD_W + expCw) + "px";
          if (rulerEl) rulerEl.style.width = expCw + "px";
          laneEls.forEach(function (ln) { ln.style.width = expCw + "px"; });
        }

        showSnapGuide(res.snapped ? s.start + curDur : null, res.type);
        var deltaSign = dd >= 0 ? "+" : "";
        var bRect = b.getBoundingClientRect();
        showDragHud(bRect.right, bRect.top,
          curDur.toFixed(2) + "s (" + deltaSign + dd.toFixed(2) + "s)" +
          (res.snapped ? (res.type === "playhead" ? " [播放头]" : " [对齐]") : ""));
      }

      var scroller = makeAutoScroller(evalRight, true);

      function restore() {
        b.style.width = w + "px";
        if (durLabel) durLabel.textContent = d0.toFixed(2) + "s";
        downstream.forEach(function (el) { el.style.transform = ""; });
        var baseCw = Math.max(220, Math.round(total * tlPPS));
        if (gridEl) gridEl.style.width = (HEAD_W + baseCw) + "px";
        if (rulerEl) rulerEl.style.width = baseCw + "px";
        laneEls.forEach(function (ln) { ln.style.width = baseCw + "px"; });
      }

      function onKey(kev) {
        if (kev.key === "Escape") {
          kev.preventDefault();
          cancelled = true;
          restore();
          cleanup();
        }
      }

      function cleanup() {
        scroller.stop();
        trimR.classList.remove("active");
        hideDragOverlays();
        window.removeEventListener("pointermove", mv);
        window.removeEventListener("pointerup", up);
        window.removeEventListener("pointercancel", cancel);
        window.removeEventListener("keydown", onKey, true);
      }

      function mv(e2) {
        if (pid !== undefined && e2.pointerId !== undefined && e2.pointerId !== pid) return;
        evalRight(e2);
        scroller.update(e2);
      }

      function cancel(e2) {
        if (pid !== undefined && e2.pointerId !== undefined && e2.pointerId !== pid) return;
        cancelled = true;
        restore();
        cleanup();
      }

      function up(e2) {
        if (pid !== undefined && e2.pointerId !== undefined && e2.pointerId !== pid) return;
        cleanup();
        if (cancelled) return;
        if (Math.abs(curDur - d0) > 0.03) {
          trimCommit(idx, curDur);
        } else {
          restore();
        }
      }

      window.addEventListener("pointermove", mv);
      window.addEventListener("pointerup", up);
      window.addEventListener("pointercancel", cancel);
      window.addEventListener("keydown", onKey, true);
    });

    // 2b. 左缘裁剪：叠加轨调整 start + duration（并顺延 video_from）；顺序主轨裁剪入点与时长
    trimL.addEventListener("pointerdown", function (e) {
      if (e.button !== 0) return;
      e.stopPropagation();
      e.preventDefault();

      var pid = e.pointerId;
      var px0 = e.clientX;
      var scEl = $("tl-scroll");
      var scroll0 = scEl ? scEl.scrollLeft : 0;
      var origStart = s.start;
      var d0 = s.dur;
      var origEnd = origStart + d0;
      var origVideoFrom = spec.video
        ? (Number(spec.video_from) ||
           (typeof spec.video === "object" ? Number(spec.video.from) || 0 : 0))
        : 0;
      var isPositioned = spec.start !== undefined || (Number(spec.track) || 0) !== 0;
      var minStart = isPositioned ? 0 : Math.max(-origVideoFrom, -60);
      if (isPositioned && spec.video) minStart = Math.max(0, origStart - origVideoFrom);
      var maxStart = origEnd - 0.3;

      var curStart = origStart;
      var curDur = d0;
      var curDelta = 0;
      var cancelled = false;
      var durLabel = b.querySelector(".dur");

      trimL.classList.add("active");
      dragShield(true);
      if (trimL.setPointerCapture && pid !== undefined) {
        try { trimL.setPointerCapture(pid); } catch { /* ignore */ }
      }

      function evalLeft(e2) {
        var dScroll = scEl ? (scEl.scrollLeft - scroll0) : 0;
        var rawStart = Math.max(minStart, Math.min(maxStart, origStart + (e2.clientX - px0 + dScroll) / tlPPS));
        if (!isPositioned && spec.video) {
          rawStart = Math.max(origStart - origVideoFrom, rawStart);
        }
        var res = e2.altKey
          ? { time: rawStart, snapped: false, type: null }
          : snapDetail(rawStart, idx, maxStart);
        var snapped = Math.max(minStart, Math.min(maxStart, res.time));
        if (!isPositioned && spec.video) {
          snapped = Math.max(origStart - origVideoFrom, snapped);
        }
        curStart = snapped;
        curDelta = curStart - origStart;
        curDur = Math.max(0.3, origEnd - curStart);

        b.style.left = (Math.max(0, curStart) * tlPPS) + "px";
        b.style.width = Math.max(14, curDur * tlPPS) + "px";
        if (durLabel) durLabel.textContent = curDur.toFixed(2) + "s";

        showSnapGuide(res.snapped && curStart >= 0 ? curStart : null, res.type);
        var bRect = b.getBoundingClientRect();
        var note = isPositioned
          ? "@" + curStart.toFixed(2) + "s · " + curDur.toFixed(2) + "s"
          : "时长 " + curDur.toFixed(2) + "s" +
            (spec.video ? " · 入点 " + Math.max(0, origVideoFrom + curDelta).toFixed(2) + "s" : "");
        showDragHud(bRect.left, bRect.top, note);
      }

      var scroller = makeAutoScroller(evalLeft);

      function restore() {
        b.style.left = (origStart * tlPPS) + "px";
        b.style.width = w + "px";
        if (durLabel) durLabel.textContent = d0.toFixed(2) + "s";
      }

      function onKey(kev) {
        if (kev.key === "Escape") {
          kev.preventDefault();
          cancelled = true;
          restore();
          cleanup();
        }
      }

      function cleanup() {
        scroller.stop();
        trimL.classList.remove("active");
        hideDragOverlays();
        window.removeEventListener("pointermove", mv);
        window.removeEventListener("pointerup", up);
        window.removeEventListener("pointercancel", cancel);
        window.removeEventListener("keydown", onKey, true);
      }

      function mv(e2) {
        if (pid !== undefined && e2.pointerId !== undefined && e2.pointerId !== pid) return;
        evalLeft(e2);
        scroller.update(e2);
      }

      function cancel(e2) {
        if (pid !== undefined && e2.pointerId !== undefined && e2.pointerId !== pid) return;
        cancelled = true;
        restore();
        cleanup();
      }

      function up(e2) {
        if (pid !== undefined && e2.pointerId !== undefined && e2.pointerId !== pid) return;
        cleanup();
        if (cancelled) return;
        if (Math.abs(curDur - d0) > 0.03) {
          trimLeftCommit(idx, {
            newDur: curDur,
            newStart: isPositioned ? curStart : undefined,
            newVideoFrom: spec.video ? Math.max(0, origVideoFrom + curDelta) : undefined
          });
        } else {
          restore();
        }
      }

      window.addEventListener("pointermove", mv);
      window.addEventListener("pointerup", up);
      window.addEventListener("pointercancel", cancel);
      window.addEventListener("keydown", onKey, true);
    });

    b.appendChild(trimL);
    b.appendChild(trimR);
  }

  // 评论角标（HF Comment 同款）：该镜留过评论即显示，点击直达 Inspector 评论编辑区
  if (spec.comment) {
    var cmt = mk("div", "cmt-badge", "💬");
    cmt.title = "评论：" + spec.comment.slice(0, 80);
    cmt.addEventListener("click", function (e) {
      e.stopPropagation();
      selectScene(idx, false);
      var ta = $("insp-comment");
      if (ta) {
        ta.scrollIntoView({ block: "center" });
        ta.focus({ preventScroll: true });
      }
    });
    b.appendChild(cmt);
  }
  lane.appendChild(b);
}

export function trackHead(td, sub, val) {
  var h = mk("div", "tl-head",
    '<div class="h-name"><span class="eye" title="显示 / 隐藏轨道">●</span>' +
    '<span class="dis">▾</span><span>' + td.name + "</span>" +
    (val ? '<span class="h-val">' + val + "</span>" : "") + "</div>" +
    '<div class="h-sub">' + sub + "</div>");
  h.querySelector(".eye").onclick = function (e) {
    e.stopPropagation();
    var row = h.parentNode, off = row.classList.toggle("off");
    this.textContent = off ? "○" : "●";
  };
  return h;
}

export function fitTimeline() {
  var view = Math.max(240, $("timeline").clientWidth - HEAD_W - 16);
  tlFitPPS = view / Math.max(state.duration, 0.1);
  tlPPS = Math.max(12, Math.min(600, tlFitPPS));
  tlFitted = true;
  renderTimeline();
}

/* 以指针位置为锚点缩放时间轴（未传入 clientX 时退回视口中点） */
export function zoomTimeline(factor, clientX) {
  var sc = $("tl-scroll");
  var rect = sc.getBoundingClientRect();
  var vpX = (typeof clientX === "number" && isFinite(clientX))
    ? Math.max(HEAD_W, clientX - rect.left)
    : sc.clientWidth / 2;
  var anchor = Math.max(0, (sc.scrollLeft + vpX - HEAD_W) / tlPPS);
  tlPPS = Math.max(12, Math.min(600, tlPPS * factor));
  renderTimeline();
  sc.scrollLeft = Math.max(0, HEAD_W + anchor * tlPPS - vpX);
  $("tl-zoom-label").textContent =
      Math.round(tlPPS / (tlFitPPS || tlPPS) * 100) + "%";
}

/* 放大到某个片段：让该片段占据视图约 70%，便于精细裁剪 / 编辑短素材。 */
export function zoomToScene(idx) {
  var s = state.scenes[idx];
  if (!s) return;
  var view = Math.max(240, $("timeline").clientWidth - HEAD_W - 16);
  var want = (view * 0.7) / Math.max(s.dur, 0.1);
  tlPPS = Math.max(12, Math.min(600, want));
  tlFitted = true;
  renderTimeline();
  var sc = $("tl-scroll");
  var center = HEAD_W + (s.start + s.dur / 2) * tlPPS;
  sc.scrollLeft = Math.max(0, center - sc.clientWidth / 2);
}

export function renderTimeline() {
  var total = Math.max(state.duration, 0.1);
  if (!tlFitted) {
    var view = Math.max(240, $("timeline").clientWidth - HEAD_W - 16);
    tlFitPPS = view / total;
    tlPPS = Math.max(12, Math.min(600, tlFitPPS));
    tlFitted = true;
  }
  var cw = Math.max(220, Math.round(total * tlPPS));
  var grid = $("tl-grid");
  grid.style.width = (HEAD_W + cw) + "px";
  grid.replaceChildren();
  lastTcHtml = lastLabel = lastBadge = lastTlTc = "";
  $("tl-zoom-label").textContent =
      Math.round(tlPPS / (tlFitPPS || tlPPS) * 100) + "%";

  /* 标尺：主刻度间隔随缩放自适应，次刻度取 1/5。 */
  var rulerRow = mk("div", "tl-row ruler-row");
  rulerRow.appendChild(mk("div", "tl-head",
    '<div class="h-name"><span class="dis">▾</span><span>TIMECODE</span></div>' +
    '<div class="h-sub">' + fps() + " fps · " + total.toFixed(2) + "s</div>"));
  var ruler = mk("div", "tl-lane tl-ruler");
  ruler.style.width = cw + "px";
  var STEPS = [0.2, 0.5, 1, 2, 5, 10, 15, 30, 60, 120];
  var step = STEPS[STEPS.length - 1];
  for (var i = 0; i < STEPS.length; i++) {
    if (STEPS[i] * tlPPS >= 78) { step = STEPS[i]; break; }
  }
  var minor = step / 5;
  for (var k = 0; k * minor <= total; k++) {
    var x = k * minor, major = (k % 5 === 0);
    var tk = mk("div", major ? "major" : "minor");
    tk.style.left = (x * tlPPS) + "px";
    if (major) tk.textContent = fmtTime(x);
    ruler.appendChild(tk);
  }
  rulerRow.appendChild(ruler);
  grid.appendChild(rulerRow);

  TRACKS.forEach(function (td, ri) {
    if (td.kind === "scene") {
      var vTracks = {};
      state.scenes.forEach(function (s, idx) {
        var tr = Number(((state.spec.scenes || [])[idx] || {}).track) || 0;
        (vTracks[tr] = vTracks[tr] || []).push(idx);
      });
      ["0", "1"].forEach(function (tr) {
        vTracks[tr] = vTracks[tr] || [];
      });
      var vKeys = Object.keys(vTracks).sort(function (a, b) { return Number(a) - Number(b); });
      vKeys.forEach(function (tr, vi) {
        var row2 = mk("div", "tl-row v-scene" + (vi % 2 ? " alt" : "") +
                      (tr !== "0" ? " overlay-row" : ""));
        row2.dataset.kind = "scene";
        row2.dataset.track = tr;
        row2.appendChild(trackHead(
          { kind: "scene",
            name: tr === "0" ? "场景 / Video" : "叠加轨 " + tr,
            row: "v-scene" },
          vTracks[tr].length + " clips · " + state.width + "×" + state.height, ""));
        var lane = mk("div", "tl-lane");
        lane.style.width = cw + "px";
        lane.dataset.kind = "scene";
        lane.dataset.track = tr;
        row2.appendChild(lane);
        grid.appendChild(row2);
        if (!vTracks[tr].length) {
          lane.appendChild(mk("div", "lane-hint",
            "＋ 拖片段到此轨自由放置时间（叠加层），或从「对话」追加场景"));
        }
        vTracks[tr].forEach(function (idx) { buildSceneClip(idx, lane, tr); });
      });
      return;
    }

    var row = mk("div", "tl-row " + td.row + (ri % 2 ? " alt" : ""));
    row.dataset.kind = td.kind;
    var audio = td.kind === "narration" || td.kind === "sfx" || td.kind === "bgm";
    var clips = audio ? state.audios.filter(function (a) {
      return a.kind === td.kind;
    }) : [];
    var sub = td.kind === "caption" ? capTrackSub()
      : clips.length + " clips · " + (td.kind === "narration" ? "voice/" : "assets/");
    row.appendChild(trackHead(td, sub, td.kind === "narration" ? "1.00" : ""));
    var lane = mk("div", "tl-lane");
    lane.style.width = cw + "px";
    lane.dataset.kind = td.kind;
    row.appendChild(lane);
    grid.appendChild(row);

    if (td.kind === "caption") {
      var segs = capSegments();
      if (segs.length) {
        segs.forEach(function (seg) {
          var a = Math.max(0, Number(seg.start) || 0);
          var z = Math.max(a + 0.05, Number(seg.end) || a + 0.05);
          var b = mk("div", "clip caption locked");
          b.style.left = (a * tlPPS) + "px";
          b.style.width = Math.max(10, (z - a) * tlPPS) + "px";
          b.textContent = String(seg.text).slice(0, 90);
          b.title = fmtTime(a) + " – " + fmtTime(z) + " · " + seg.text;
          b.onclick = function () { seekTo(a + 0.01); };
          lane.appendChild(b);
        });
      } else {
        var texts = sceneTexts();
        (state.spec.scenes || []).forEach(function (spec, idx) {
          var s = state.scenes[idx];
          if (!s) return;
          var text = texts[idx];
          if (!text) return;
          var b2 = mk("div", "clip caption locked");
          b2.style.left = (s.start * tlPPS) + "px";
          b2.style.width = Math.max(16, s.dur * tlPPS) + "px";
          b2.textContent = String(text).slice(0, 90);
          b2.title = "字幕 / 场景文本（spec）";
          b2.onclick = function () { seekTo(s.start + 0.01); };
          lane.appendChild(b2);
        });
      }
    } else {
      clips.forEach(function (a) {
        var w = Math.max(14, Math.min(a.dur || total, total) * tlPPS);
        var b = mk("div", "clip audio locked" + (td.kind === "narration" ? "" : " slim"));
        b.style.left = (a.start * tlPPS) + "px";
        b.style.width = w + "px";
        var nm = a.src.split("/").pop();
        var wh = td.kind === "narration" ? 32 : 26;
        b.innerHTML = '<div class="cap">' + nm +
          '<span class="dur">v' + a.vol.toFixed(1) + "</span></div>" +
          '<div class="wave">' + waveSVG(hashSeed(a.src), w, wh,
            td.kind === "narration") + "</div>";
        b.title = a.src + " · " + (a.dur || 0).toFixed(2) + "s";
        b.onclick = function () { seekTo(a.start + 0.01); };
        lane.appendChild(b);
        loadWaveForClip(b.querySelector(".wave"), a.src, w, wh,
                        td.kind === "narration", a.media_start || 0, a.dur || 0);
      });
    }
  });

  var ph = mk("div", "", '<span class="ph-tc"></span>');
  ph.id = "playhead";
  grid.appendChild(ph);

  ensureRangeOverlay();
  applyRangeGeom();
  syncRangeUI();
  if (!playing) phVisual = lastTime;
  updatePlayhead(phVisual, total);
  if (playing) startPhAnim(phVisual);
}

/* ---------------- 全局标尺 / 空白区 Scrub 与滚轮居中缩放 ---------------- */
(function () {
  function scrubAt(ev) {
    var rect = $("tl-grid").getBoundingClientRect();
    var total = Math.max(state.duration, 0.1);
    var t = Math.max(0, Math.min(total, (ev.clientX - rect.left - HEAD_W) / tlPPS));
    var val = ev.altKey
      ? Math.round(t * fps()) / fps()
      : (tlSnap ? snapTime(t) : Math.round(t * fps()) / fps());
    seekTo(val);
  }

  $("timeline").addEventListener("pointerdown", function (ev) {
    if (ev.button !== 0) return;
    if (ev.target.closest(".tl-head") || ev.target.closest(".tl-ih")) return;
    var onClip = ev.target.closest(".clip");
    var onTrim = ev.target.closest(".trim");
    if (!onTrim && !onClip && (tlRangeMode || ev.shiftKey)) {
      startRangeDrag(ev);
      return;
    }
    // 允许直接拖标尺、#playhead 顶盖、或非场景空白轨道，不抢场景 clip 的拖动
    if (onClip && !ev.target.closest(".tl-ruler") && ev.target.id !== "playhead") return;

    var pid = ev.pointerId;
    var tl = $("timeline");
    dragShield(true);
    if (tl && tl.setPointerCapture && pid !== undefined) {
      try { tl.setPointerCapture(pid); } catch { /* ignore */ }
    }
    scrubAt(ev);
    var scroller = makeAutoScroller(scrubAt);

    function mv(e2) {
      if (pid !== undefined && e2.pointerId !== undefined && e2.pointerId !== pid) return;
      scrubAt(e2);
      scroller.update(e2);
    }
    function up(e2) {
      if (pid !== undefined && e2.pointerId !== undefined && e2.pointerId !== pid) return;
      scroller.stop();
      dragShield(false);
      window.removeEventListener("pointermove", mv);
      window.removeEventListener("pointerup", up);
      window.removeEventListener("pointercancel", up);
    }
    window.addEventListener("pointermove", mv);
    window.addEventListener("pointerup", up);
    window.addEventListener("pointercancel", up);
  });

  $("timeline").addEventListener("wheel", function (ev) {
    if (ev.ctrlKey || ev.metaKey) {
      ev.preventDefault();
      zoomTimeline(ev.deltaY < 0 ? 1.25 : 0.8, ev.clientX);
      return;
    }
    if (ev.shiftKey && ev.deltaY && !ev.deltaX) {
      ev.preventDefault();
      var sc = $("tl-scroll");
      if (sc) sc.scrollLeft += ev.deltaY;
    }
  }, { passive: false });
})();

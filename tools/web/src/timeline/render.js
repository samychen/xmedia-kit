import { selectScene } from "../panels/scenes.js";
import { seekTo, updatePlayhead } from "../preview/driver.js";
import { startPhAnim } from "../preview/playhead.js";
import { log, $ } from "../shared/dom.js";
import { HEAD_W } from "../shared/state.js";
import { fmtTime, fps } from "../shared/time.js";
import { TRACKS, capSegments, capTrackSub, hashSeed, loadThumbsForClip, loadWaveForClip, mk, sceneTexts, snapTime, stripHTML, waveSVG } from "./model.js";
import { applyRangeGeom, ensureRangeOverlay, startRangeDrag, syncRangeUI } from "./range.js";
import { reorder, reorderTo, splitScene, trimCommit } from "./trim.js";

/* 拖拽插入位判定：鼠标时间 t 落在哪条边界之前 → 插入位下标。
   边界取各场景中点（拖到某场景前半 = 放它之前，后半 = 放它之后），
   被拖场景自身跳过。 */
function dropIndex(t, dragIdx) {
  var sc = state.scenes || [];
  for (var i = 0; i < sc.length; i++) {
    if (i === dragIdx) continue;
    if (t < sc[i].start + sc[i].dur / 2) return i;
  }
  return sc.length;
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
export function zoomTimeline(factor) {
  var sc = $("tl-scroll");
  var anchor = (sc.scrollLeft + sc.clientWidth / 2 - HEAD_W) / tlPPS;
  tlPPS = Math.max(12, Math.min(600, tlPPS * factor));
  renderTimeline();
  sc.scrollLeft = Math.max(0,
    HEAD_W + anchor * tlPPS - sc.clientWidth / 2);
  $("tl-zoom-label").textContent =
      Math.round(tlPPS / (tlFitPPS || tlPPS) * 100) + "%";
}
/* 放大到某个片段：让该片段占据视图约 70%，便于精细裁剪 / 编辑短素材。
   双击片段或点工具栏「所选」触发；不改变其他片段的逻辑。 */
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
  // 轨道 DOM 重建后清掉写入缓存，否则新元素会因「值没变」而留空。
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
    var row = mk("div", "tl-row " + td.row + (ri % 2 ? " alt" : ""));
    row.dataset.kind = td.kind;
    var audio = td.kind === "narration" || td.kind === "sfx" ||
                td.kind === "bgm";
    var clips = audio ? state.audios.filter(function (a) {
      return a.kind === td.kind;
    }) : [];
    var sub = td.kind === "scene"
      ? state.scenes.length + " clips · " + state.width + "×" + state.height
      : td.kind === "caption" ? capTrackSub()
      : clips.length + " clips · " +
        (td.kind === "narration" ? "voice/" : "assets/");
    row.appendChild(trackHead(td, sub,
      td.kind === "narration" ? "1.00" : ""));
    var lane = mk("div", "tl-lane");
    lane.style.width = cw + "px";
    lane.dataset.kind = td.kind;
    row.appendChild(lane);
    grid.appendChild(row);

    if (td.kind === "scene") {
      state.scenes.forEach(function (s, idx) {
        var spec = (state.spec.scenes || [])[idx] || {};
        var locked = !!spec.narration;
        var w = Math.max(14, s.dur * tlPPS);
        var b = mk("div", "clip scene" + (locked ? " locked" : "") +
                   (idx === selIdx ? " sel" : ""));
        b.style.left = (s.start * tlPPS) + "px";
        b.style.width = w + "px";
        b.title = (locked ? "旁白锁定：时长随旁白拟合"
                          : "点选；拖右缘裁剪时长；右键在播放头处切割") +
                  " · " + s.label;
        b.innerHTML = '<div class="cap">' + (idx + 1) + " · " + s.label +
          '<span class="dur">' + s.dur.toFixed(2) + "s</span></div>" +
          stripHTML(s, idx, w);
        if (spec.video) {
          var _strip = b.querySelector(".strip");
          if (_strip) {
            loadThumbsForClip(_strip, spec.video,
                              spec.video_from || 0, s.dur);
          }
        }
        b.onclick = function (ev) {
          var cn = ev.target.className || "";
          if (cn.indexOf("trim") >= 0 || cn.indexOf("mv") >= 0) return;
          if (b._dragged) { b._dragged = false; return; }  // 拖拽后抑制误选
          selectScene(selIdx === idx ? -1 : idx, false);
        };
        // 拖拽重排手势（HF 的拖 clip 换位）：非裁剪区按住水平拖过 6px
        // 阈值 → clip 跟随 + 插入位指示线 → 松手 reorderTo 落位重组。
        // 重排不改变时长/旁白——旁白锁定场景同样可拖（锁定只约束
        // 裁剪与文案）。
        b.onmousedown = function (e) {
          if (e.button !== 0) return;
          if (e.target.className &&
              String(e.target.className).indexOf("trim") >= 0) return;
          var px0 = e.clientX, dragging = false, target = -1;
          function mv(e2) {
            var dx = e2.clientX - px0;
            if (!dragging && Math.abs(dx) < 6) return;
            if (!dragging) {
              dragging = true;
              b._dragging = true;
              b.classList.add("dragging");
              lane._drop = mk("div", "drop-line");
              lane.appendChild(lane._drop);
            }
            b.style.transform = "translateX(" + dx + "px)";
            var t = (e2.clientX -
                     lane.getBoundingClientRect().left) / tlPPS;
            target = dropIndex(t, idx);
            var ex = target < state.scenes.length ?
                state.scenes[target].start * tlPPS :
                (state.scenes[state.scenes.length - 1].start +
                 state.scenes[state.scenes.length - 1].dur) * tlPPS;
            lane._drop.style.left = ex + "px";
          }
          function up() {
            document.removeEventListener("mousemove", mv);
            document.removeEventListener("mouseup", up);
            b.classList.remove("dragging");
            b.style.transform = "";
            if (lane._drop) { lane._drop.remove(); lane._drop = null; }
            if (dragging) {
              b._dragged = true;
              setTimeout(function () { b._dragged = false; }, 250);
              if (target >= 0) reorderTo(idx, target);
            }
          }
          document.addEventListener("mousemove", mv);
          document.addEventListener("mouseup", up);
        };
        b.ondblclick = function () { selectScene(idx, false); zoomToScene(idx); };
        // 右键 = 在播放头处切开这一段（刀口不在片段内时给日志提示）。
        b.oncontextmenu = function (ev) {
          ev.preventDefault();
          if (locked) {
            log("scene-" + idx + " 旁白锁定，不支持切割", "err");
            return;
          }
          selectScene(idx, false);
          splitScene(idx, lastTime);
        };
        if (!locked) {
          var trim = mk("div", "trim");
          trim.onmousedown = function (e) {
            e.stopPropagation();
            var px0 = e.clientX, d0 = s.dur;
            function mv(e2) {
              var d = d0 + (e2.clientX - px0) / tlPPS;
              d = Math.max(0.3, Math.min(d, total - s.start));
              b.style.width = Math.max(14, d * tlPPS) + "px";
              b._newDur = d;
            }
            function up() {
              document.removeEventListener("mousemove", mv);
              document.removeEventListener("mouseup", up);
              if (b._newDur && Math.abs(b._newDur - d0) > 0.05) {
                trimCommit(idx, b._newDur);
              }
            }
            document.addEventListener("mousemove", mv);
            document.addEventListener("mouseup", up);
          };
          b.appendChild(trim);
        }
        if (idx === selIdx && !locked) {
          var mvL = mk("div", "mv left", "‹");
          mvL.title = "前移一位";
          mvL.onclick = function (e) {
            e.stopPropagation(); reorder(idx, -1);
          };
          var mvR = mk("div", "mv right", "›");
          mvR.title = "后移一位";
          mvR.onclick = function (e) {
            e.stopPropagation(); reorder(idx, 1);
          };
          b.appendChild(mvL); b.appendChild(mvR);
        }
        // 评论角标（HF Comment 同款）：该镜留过评论即显示，点击直达
        // Inspector 的评论编辑区。
        if (spec.comment) {
          var cmt = mk("div", "cmt-badge", "💬");
          cmt.title = "评论：" + spec.comment.slice(0, 80);
          cmt.onclick = function (e) {
            e.stopPropagation();
            selectScene(idx, false);
            var ta = $("insp-comment");
            if (ta) {
              ta.scrollIntoView({ block: "center" });
              ta.focus({ preventScroll: true });
            }
          };
          b.appendChild(cmt);
        }
        lane.appendChild(b);
      });
    } else if (td.kind === "caption") {
      var segs = capSegments();
      if (segs.length) {
        // ASR / 外部字幕：按真实时间戳一段一块
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
        var b = mk("div", "clip audio locked" +
                   (td.kind === "narration" ? "" : " slim"));
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
        // 真实峰值是异步来的（ffmpeg 抽一次，之后走服务端缓存）。
        // 拿到就替换示意波形；没有音轨则保持原样。
        loadWaveForClip(b.querySelector(".wave"), a.src, w, wh,
                        td.kind === "narration", a.media_start || 0, a.dur || 0);
      });
    }
  });
  var ph = mk("div", "", '<span class="ph-tc"></span>');
  ph.id = "playhead";
  grid.appendChild(ph);
  // 区间覆盖层：整块重排后必须重建（innerHTML 清空会把旧节点连同选区一起丢掉）。
  ensureRangeOverlay();
  applyRangeGeom();
  syncRangeUI();
  if (!playing) phVisual = lastTime;  // 暂停态恢复真实位置
  updatePlayhead(phVisual, total);
  // 播放头元素被重建，必须重新挂上合成层过渡，否则会停在起点。
  if (playing) startPhAnim(phVisual);
}


















































































































(function () {
  function scrubAt(ev) {
    var rect = $("tl-grid").getBoundingClientRect();
    var total = Math.max(state.duration, 0.1);
    var t = Math.max(0, Math.min(total,
      (ev.clientX - rect.left - HEAD_W) / tlPPS));
    seekTo(tlSnap ? snapTime(t) : Math.round(t * fps()) / fps());
  }
  $("timeline").addEventListener("mousedown", function (ev) {
    if (ev.target.closest(".tl-head") || ev.target.closest(".tl-ih")) return;
    var onTrim = ev.target.closest(".trim") || ev.target.closest(".mv");
    if (!onTrim && (tlRangeMode || ev.shiftKey)) { startRangeDrag(ev); return; }
    if (ev.target.closest(".clip")) return;
    scrubAt(ev);
    function mv(e2) { scrubAt(e2); }
    function up() {
      document.removeEventListener("mousemove", mv);
      document.removeEventListener("mouseup", up);
    }
    document.addEventListener("mousemove", mv);
    document.addEventListener("mouseup", up);
  });
  $("timeline").addEventListener("wheel", function (ev) {
    if (!(ev.ctrlKey || ev.metaKey)) return;
    ev.preventDefault();
    zoomTimeline(ev.deltaY < 0 ? 1.25 : 0.8);
  }, { passive: false });
})();

import { seekTo } from "../preview/driver.js";
import { log, $ } from "../shared/dom.js";
import { HEAD_W } from "../shared/state.js";
import { fmtTime, fps } from "../shared/time.js";
import { snapTime } from "./model.js";

/* ---------------- 时间轴区间（in / out） ----------------
   NLE 惯例：选区外压暗、选区内保持原色；两端把手可拖，标尺上有一道高亮带。
   选区只影响预览与播放范围，不改 spec —— 破坏性裁剪仍走单片段拖右缘。 */
export function tlTotal() { return Math.max(state.duration, 0.1); }
export function tlCw() { return Math.max(220, Math.round(tlTotal() * tlPPS)); }
export function rangeOpen() {
  return !!tlRange && (tlRange.b - tlRange.a) >= 0.5 / fps();
}
export function rangeTimeAt(ev) {
  var rect = $("tl-grid").getBoundingClientRect();
  return Math.max(0, Math.min(tlTotal(),
    (ev.clientX - rect.left - HEAD_W) / tlPPS));
}
export function rangeClamp(r) {
  var total = tlTotal();
  var a = Math.max(0, Math.min(total, Number(r.a) || 0));
  var b = Math.max(0, Math.min(total, Number(r.b) || 0));
  if (b < a) { var t = a; a = b; b = t; }
  return { a: a, b: b };
}
export function rangeLabel() {
  if (!rangeOpen()) return "无选区";
  var d = tlRange.b - tlRange.a;
  return fmtTime(tlRange.a) + " – " + fmtTime(tlRange.b) + " · " +
         d.toFixed(2) + "s · " + Math.round(d * fps()) + " 帧";
}
/* 覆盖层只在 renderTimeline 重排后重建一次；拖拽过程只改几何，不整块重绘
   （重绘会重建全部片段与缩略图，拖动时必然掉帧）。 */
export function ensureRangeOverlay() {
  var g = $("tl-grid");
  if (!g) return;
  var old = $("tl-dim-l");
  if (old && old.parentNode === g) return;
  function el(id, cls) {
    var e = document.createElement("div");
    if (id) e.id = id;
    if (cls) e.className = cls;
    return e;
  }
  g.appendChild(el("tl-dim-l"));
  g.appendChild(el("tl-dim-r"));
  g.appendChild(el("tl-range-body"));
  g.appendChild(el("tl-range-ruler"));
  var hl = el("", "tl-ih left"), hr = el("", "tl-ih right");
  hl.appendChild(el("", "tc"));
  hr.appendChild(el("", "tc"));
  hl.onmousedown = function (ev) { dragRangeHandle(ev, "a"); };
  hr.onmousedown = function (ev) { dragRangeHandle(ev, "b"); };
  g.appendChild(hl);
  g.appendChild(hr);
}
export function applyRangeGeom() {
  var g = $("tl-grid");
  if (!g) return;
  var hl = g.querySelector(".tl-ih.left");
  var hr = g.querySelector(".tl-ih.right");
  var parts = ["tl-dim-l", "tl-dim-r", "tl-range-body", "tl-range-ruler"];
  var has = rangeOpen();
  if (!has) {
    parts.forEach(function (id) {
      var e = $(id); if (e) e.style.display = "none";
    });
    if (hl) hl.style.display = "none";
    if (hr) hr.style.display = "none";
    return;
  }
  var cw = tlCw();
  var x1 = HEAD_W + tlRange.a * tlPPS;
  var x2 = HEAD_W + tlRange.b * tlPPS;
  var w = Math.max(1, x2 - x1);
  parts.forEach(function (id) {
    var e = $(id); if (e) e.style.display = "";
  });
  var dl = $("tl-dim-l"), dr = $("tl-dim-r");
  var body = $("tl-range-body"), band = $("tl-range-ruler");
  if (dl) {
    dl.style.left = HEAD_W + "px";
    dl.style.width = Math.max(0, x1 - HEAD_W) + "px";
  }
  if (dr) {
    dr.style.left = x2 + "px";
    dr.style.width = Math.max(0, HEAD_W + cw - x2) + "px";
  }
  if (body) { body.style.left = x1 + "px"; body.style.width = w + "px"; }
  if (band) { band.style.left = x1 + "px"; band.style.width = w + "px"; }
  if (hl) {
    hl.style.display = "";
    hl.style.left = (x1 - 6) + "px";
    hl.querySelector(".tc").textContent = fmtTime(tlRange.a);
    hl.querySelector(".tc").style.display = w < 96 ? "none" : "";
  }
  if (hr) {
    hr.style.display = "";
    hr.style.left = (x2 - 6) + "px";
    hr.querySelector(".tc").textContent = fmtTime(tlRange.b);
    hr.querySelector(".tc").style.display = w < 96 ? "none" : "";
  }
}
export function syncRangeUI() {
  var box = $("tl-range-box");
  if (box) box.hidden = !rangeOpen();
  var info = $("tl-range-info");
  if (info) info.textContent = rangeLabel();
  var lp = $("tl-range-loop");
  if (lp) lp.classList.toggle("on", rangeLoop);
  var md = $("tl-range-mode");
  if (md) md.classList.toggle("on", tlRangeMode);
  var tl = $("timeline");
  if (tl) tl.classList.toggle("range-mode", tlRangeMode);
}
export function pushRange() {
  var w = $("pv").contentWindow;
  if (!w) return;
  var payload = rangeOpen()
      ? { a: tlRange.a, b: tlRange.b, loop: rangeLoop } : null;
  if (w.__mcStudioAPI && w.__mcStudioAPI.setRange) {
    w.__mcStudioAPI.setRange(payload);
  } else {
    w.postMessage({ mcStudioCommand: "range", mcStudioValue: payload }, location.origin);
  }
}
export function setRange(a, b, quiet) {
  tlRange = rangeClamp({ a: a, b: b });
  if (!rangeOpen()) tlRange = null;
  ensureRangeOverlay();
  applyRangeGeom();
  syncRangeUI();
  pushRange();
  if (!quiet && rangeOpen()) log("选区 " + rangeLabel());
}
export function clearRange(quiet) {
  if (!tlRange) return;
  tlRange = null;
  applyRangeGeom();
  syncRangeUI();
  pushRange();
  if (!quiet) log("已清除选区");
}
export function dragRangeHandle(ev, which) {
  if (!rangeOpen()) return;
  ev.preventDefault();
  ev.stopPropagation();
  var minGap = 1 / fps();
  function mv(e2) {
    var t = rangeTimeAt(e2);
    if (tlSnap) t = snapTime(t);
    var r = { a: tlRange.a, b: tlRange.b };
    if (which === "a") r.a = Math.max(0, Math.min(t, r.b - minGap));
    else r.b = Math.min(tlTotal(), Math.max(t, r.a + minGap));
    tlRange = r;
    applyRangeGeom();
    syncRangeUI();
  }
  function up() {
    document.removeEventListener("mousemove", mv);
    document.removeEventListener("mouseup", up);
    pushRange();
    log("选区 " + rangeLabel());
  }
  document.addEventListener("mousemove", mv);
  document.addEventListener("mouseup", up);
}
/* 从按下点拖出一个新区间；不足 1.5 帧视作单击定位。 */
export function startRangeDrag(ev) {
  var t0 = rangeTimeAt(ev);
  if (tlSnap) t0 = snapTime(t0);
  var moved = false;
  tlRange = { a: t0, b: t0 };
  ensureRangeOverlay();
  applyRangeGeom();
  syncRangeUI();
  function mv(e2) {
    var t = rangeTimeAt(e2);
    if (tlSnap) t = snapTime(t);
    if (Math.abs(t - t0) > 1.5 / fps()) moved = true;
    tlRange = rangeClamp({ a: t0, b: t });
    applyRangeGeom();
    syncRangeUI();
  }
  function up() {
    document.removeEventListener("mousemove", mv);
    document.removeEventListener("mouseup", up);
    if (!moved || !rangeOpen()) {
      tlRange = null;
      applyRangeGeom();
      syncRangeUI();
      pushRange();
      seekTo(t0);
      return;
    }
    pushRange();
    log("选区 " + rangeLabel());
  }
  document.addEventListener("mousemove", mv);
  document.addEventListener("mouseup", up);
}

/* ---------------- Draw 手绘标注（HF 的画布手绘同款语义） ----------------
 * 预览区叠加 canvas：激活画笔后直接在画面上画标注——**标注进成片**：
 * 笔划按画面（pvframe）归一化收集进 spec.annotations，防抖 1.2s 提交
 * recompose（契约页生成 SVG 标注层）→ 预览与导出都带标注。画布上的
 * 笔迹在提交后清空（由契约页 SVG 接管显示，避免双份）。 */
import { $, log } from "../shared/dom.js";
import { api } from "../shared/api-client.js";
import { refresh } from "../panels/status.js";

let cv = null, ctx = null, drawing = false, on = false;
let last = null;
let curStroke = null;          // 当前笔划（归一化点数组）
let pendingStrokes = [];       // 待提交笔划
let commitTimer = null;

function frameRect() {
  var f = document.querySelector("#pvframe");
  return f ? f.getBoundingClientRect() : null;
}

function ensureCanvas() {
  if (cv) return cv;
  cv = document.createElement("canvas");
  cv.id = "draw-canvas";
  cv.style.cssText = "position:absolute;left:0;top:0;width:100%;height:100%;"
      + "z-index:8;display:none;cursor:crosshair;touch-action:none;";
  $("pvwrap").appendChild(cv);
  ctx = cv.getContext("2d");
  ctx.lineCap = "round";
  ctx.lineJoin = "round";
  ctx.strokeStyle = "#ff4d9d";
  ctx.lineWidth = 3;
  /* 画笔事件：pointer 系列统一（鼠标/触控板/触屏） */
  cv.addEventListener("pointerdown", function (e) {
    if (!on) return;
    drawing = true;
    last = pos(e);
    curStroke = { points: [norm(e)], color: "#ff4d9d", width: 4 };
    try { cv.setPointerCapture(e.pointerId); } catch (err) { /* 无所谓：capture 只是拖出画布也持续收点 */ }
  });
  cv.addEventListener("pointermove", function (e) {
    if (!on || !drawing) return;
    var p = pos(e);
    ctx.beginPath();
    ctx.moveTo(last.x, last.y);
    ctx.lineTo(p.x, p.y);
    ctx.stroke();
    last = p;
    curStroke.points.push(norm(e));
  });
  ["pointerup", "pointerleave", "pointercancel"].forEach(function (ev) {
    cv.addEventListener(ev, function () {
      if (drawing && curStroke && curStroke.points.length > 1) {
        pendingStrokes.push(curStroke);
        scheduleCommit();
      }
      drawing = false;
      curStroke = null;
    });
  });
  return cv;
}

/* 画笔点 → 画面归一化坐标（0-1，以 pvframe 为基准，黑边笔迹裁掉） */
function norm(e) {
  var f = frameRect();
  if (!f) return [0, 0];
  return [
    Math.max(0, Math.min(1, (e.clientX - f.left) / f.width)),
    Math.max(0, Math.min(1, (e.clientY - f.top) / f.height))
  ];
}

/* 笔划提交：防抖 1.2s 合并多笔划 → spec.annotations → recompose
   （契约页 SVG 接管显示）→ 画布清空。 */
function scheduleCommit() {
  if (commitTimer) clearTimeout(commitTimer);
  commitTimer = setTimeout(function () {
    commitTimer = null;
    if (!pendingStrokes.length) return;
    var strokes = pendingStrokes;
    pendingStrokes = [];
    /* 服务端原子 append：连续快速笔划不再互相覆盖丢笔划 */
    api("/api/annotation/add", { strokes: strokes })
      .then(function (r) {
        drawClear();
        return refresh().then(function () {
          log("标注已画入视频（共 " + r.count + " 笔）", "ok");
        });
      })
      .catch(function (e) {
        log("标注提交失败：" + e.message, "err");
      });
  }, 1200);
}

function resize() {
  if (!cv) return;
  var r = cv.getBoundingClientRect();
  if (r.width < 2 || r.height < 2) return;  // 布局未就绪（rAF 后重试）
  cv.width = Math.max(2, Math.round(r.width));
  cv.height = Math.max(2, Math.round(r.height));
  ctx.lineCap = "round";
  ctx.lineJoin = "round";
}

function pos(e) {
  var r = cv.getBoundingClientRect();
  return { x: e.clientX - r.left, y: e.clientY - r.top };
}

export function drawToggle(on_) {
  on = !!on_;
  var c = ensureCanvas();
  c.style.display = on ? "block" : "none";
  if (on) {
    /* 布局 flush 后再取尺寸——同 tick 内 rect 是 canvas 默认 300×150，
       内容会被 CSS 拉伸错位（用户观感"画了没反应"） */
    requestAnimationFrame(resize);
    log("画笔已激活——画在画面上的标注将导出到视频", "ok");
  }
}

export function drawIsOn() { return on; }

export function drawClear() {
  if (ctx && cv) ctx.clearRect(0, 0, cv.width, cv.height);
}

export function drawInit() {
  var btn = $("pvtool-draw");
  if (!btn) return;
  window.addEventListener("resize", resize);
  btn.onclick = function () {
    on = !on;
    this.classList.toggle("on", on);
    drawToggle(on);
    var clr = $("pvtool-clear");
    if (clr) clr.style.display = on ? "" : "none";
  };
  var clearBtn = $("pvtool-clear");
  if (clearBtn) clearBtn.onclick = function () {
    if (commitTimer) {
      clearTimeout(commitTimer);
      commitTimer = null;
    }
    pendingStrokes = [];
    drawClear();
    api("/api/annotation/clear", {}).then(function () {
      return refresh().then(function () {
        log("标注已清除", "ok");
      });
    }).catch(function (e) {
      log("清除失败：" + e.message, "err");
    });
  };
}

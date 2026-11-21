/* ---------------- Draw 手绘标注（HF 的画布手绘同款语义） ----------------
 * 预览区叠加 canvas：激活画笔后直接在画面上画标注（表达修改意图），
 * 会话级（不进 spec/成片）。坐标 = canvas 内部像素，随窗口缩放重设
 * 尺寸时清空（标注是临时笔迹）。 */
import { $, log } from "../shared/dom.js";

let cv = null, ctx = null, drawing = false, on = false;
let last = null;

function ensureCanvas() {
  if (cv) return cv;
  cv = document.createElement("canvas");
  cv.id = "draw-canvas";
  cv.style.cssText = "position:absolute;inset:0;z-index:8;display:none;" +
      "cursor:crosshair;touch-action:none;";
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
    cv.setPointerCapture(e.pointerId);
  });
  cv.addEventListener("pointermove", function (e) {
    if (!on || !drawing) return;
    var p = pos(e);
    ctx.beginPath();
    ctx.moveTo(last.x, last.y);
    ctx.lineTo(p.x, p.y);
    ctx.stroke();
    last = p;
  });
  ["pointerup", "pointerleave", "pointercancel"].forEach(function (ev) {
    cv.addEventListener(ev, function () { drawing = false; });
  });
  return cv;
}

function resize() {
  if (!cv || !cv.style.display || cv.style.display === "none") return;
  var r = cv.getBoundingClientRect();
  cv.width = Math.max(2, r.width);
  cv.height = Math.max(2, r.height);
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
    resize();
    log("画笔已激活——直接在画面上画标注（表达修改意图，会话级）", "ok");
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
  if (clearBtn) clearBtn.onclick = function () { drawClear(); };
}

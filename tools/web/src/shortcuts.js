import { selectScene } from "./panels/scenes.js";
import { recompose, refresh } from "./panels/status.js";
import { handleState, pvCmd, renderAudioBtn } from "./preview/driver.js";
import { fitPreview, zoomStep } from "./preview/zoom.js";
import { log, $ } from "./shared/dom.js";
import { fps } from "./shared/time.js";
import { snapTime } from "./timeline/model.js";
import { applyRangeGeom, clearRange, ensureRangeOverlay, pushRange, syncRangeUI, tlTotal } from "./timeline/range.js";
import { splitAtPlayhead } from "./timeline/trim.js";

export function shortcut(code, shift, meta, ctrl, editable) {
  if ((meta || ctrl) && code === "Enter") { recompose(); return true; }
  if (editable) return false;
  if (code === "Space") { pvCmd("toggle"); return true; }
  // 切割：S 在播放头处下刀（⌘/Ctrl+S 留给浏览器保存，不拦）
  if (code === "KeyS" && !meta && !ctrl) { splitAtPlayhead(); return true; }
  if (code === "KeyM") {
    audioOn = !audioOn;
    pvCmd("setMuted", !audioOn);
    renderAudioBtn();
    return true;
  }
  if (code === "ArrowLeft") { pvCmd("step", shift ? -10 : -1); return true; }
  if (code === "ArrowRight") { pvCmd("step", shift ? 10 : 1); return true; }
  if (code === "Home") { pvCmd("goto", 0); return true; }
  if (code === "End") {
    pvCmd("goto", Math.max(0, state.duration - 1 / fps())); return true;
  }
  // 区间打点：I = in，O = out（以播放头为基准，遵守吸附）。
  if (code === "KeyI" || code === "KeyO") {
    var at = snapTime(Math.max(0, Math.min(tlTotal(), lastTime)));
    var gap = 1 / fps();
    if (code === "KeyI") {
      if (!tlRange) tlRange = { a: at, b: Math.min(tlTotal(), at + gap) };
      else tlRange.a = at;
      if (tlRange.b <= tlRange.a) {
        tlRange.b = Math.min(tlTotal(), tlRange.a + gap);
      }
    } else {
      if (!tlRange) tlRange = { a: Math.max(0, at - gap), b: at };
      else tlRange.b = at;
      if (tlRange.b <= tlRange.a) tlRange.a = Math.max(0, tlRange.b - gap);
    }
    ensureRangeOverlay();
    applyRangeGeom();
    syncRangeUI();
    pushRange();
    return true;
  }
  if (code === "Escape") {
    if (tlRange) { clearRange(); return true; }
  }
  if (code === "Equal" || code === "NumpadAdd") { zoomStep(1); return true; }
  if (code === "Minus" || code === "NumpadSubtract") {
    zoomStep(-1); return true;
  }
  return false;
}
document.addEventListener("keydown", function (ev) {
  var tag = (ev.target.tagName || "").toLowerCase();
  var editable = tag === "input" || tag === "textarea" || tag === "select";
  if (shortcut(ev.code, ev.shiftKey, ev.metaKey, ev.ctrlKey, editable)) {
    ev.preventDefault();
  }
});
window.addEventListener("resize", fitPreview);
window.addEventListener("message", function (ev) {
  /* 预览 iframe 与本页同源（/preview 注入）；跨源消息一律丢弃。 */
  if (ev.origin !== location.origin) return;
  var d = ev.data || {};
  if (d.mcStudioState || typeof d.mcStudioTime === "number") {
    handleState(d);
  }
  if (typeof d.mcStudioSelect === "number" &&
      d.mcStudioSelect < ((state.spec.scenes || []).length)) {
    selectScene(d.mcStudioSelect, false);
    /* 元素级画布：预览里点中的元素 → Inspector 定位对应字段并短暂
       高亮（HF 的"点画面直接改"闭环）。字段可能不存在（类型不同/
       自定义模板），静默忽略。 */
    var f = d.mcStudioField;
    if (f) {
      var el = $("insp-f-" + f);
      if (el && f !== "narration") {
        el.scrollIntoView({ block: "center", behavior: "smooth" });
        el.focus({ preventScroll: true });
        el.classList.add("insp-flash");
        setTimeout(function () { el.classList.remove("insp-flash"); }, 900);
      }
    }
  }
  if (d.mcStudioKey) {
    shortcut(d.mcStudioKey, d.shiftKey, d.metaKey, d.ctrlKey, false);
  }
});
$("pv").addEventListener("load", function () { fitPreview(); pushRange(); });
refresh().then(function () {
  if (window.ResizeObserver) {
    new ResizeObserver(function () { fitPreview(); }).observe($("pvwrap"));
  }
  /* docking 入场：Home 创建流（?fresh=1）跳进编辑器的一次性仪式感。 */
  if (location.search.indexOf("fresh=1") >= 0) {
    document.body.classList.add("docking");
    setTimeout(function () { document.body.classList.remove("docking"); },
               1400);
    history.replaceState(null, "", location.pathname);
  }
}).catch(function (e) { log("状态加载失败：" + e.message, "err"); });

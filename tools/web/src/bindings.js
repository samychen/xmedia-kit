import { $ } from "./shared/dom.js";
import { scriptRender } from "./panels/script.js";
import { pvCmd, renderAudioBtn, seekTo, unlockPreviewAudio } from "./preview/driver.js";
import { phFrame } from "./preview/playhead.js";
import { fitPreview } from "./preview/zoom.js";
import { ICON } from "./shared/icons.js";
import { fps } from "./shared/time.js";
import { clearRange, pushRange, rangeOpen, syncRangeUI } from "./timeline/range.js";
import { fitTimeline, zoomTimeline, zoomToScene } from "./timeline/render.js";
import { splitAtPlayhead } from "./timeline/trim.js";
import { storyboardIsOpen, storyboardOpen, storyboardRender } from "./panels/storyboard.js";
import { tplRender } from "./panels/templates.js";
import { brandRender } from "./panels/brand.js";
import { projectsRender } from "./panels/projects.js";
import { sbGridShow } from "./preview/sbgrid.js";
import { drawInit } from "./preview/draw.js";

/* ---------------- 交互绑定 ---------------- */
$("btn-play").innerHTML = ICON.play;
renderAudioBtn();
phRaf = requestAnimationFrame(phFrame);  // 播放头自持的匀速视觉时钟
$("btn-play").onclick = function () { pvCmd("toggle"); };
$("btn-first").onclick = function () { pvCmd("goto", 0); };
$("btn-last").onclick = function () {
  pvCmd("goto", Math.max(0, state.duration - 1 / fps()));
};
$("btn-prev").onclick = function () { pvCmd("step", -1); };
$("btn-next").onclick = function () { pvCmd("step", 1); };
$("btn-loop").onclick = function () {
  loopOn = !loopOn;
  $("btn-loop").classList.toggle("on", loopOn);
  pvCmd("loop", loopOn);
};
$("rate").onchange = function () { pvCmd("rate", parseFloat(this.value)); };
$("btn-audio").onclick = function () {
  audioOn = !audioOn;
  pvCmd("setMuted", !audioOn);
  renderAudioBtn();
};
$("vol").addEventListener("input", function () {
  pvCmd("setVolume", parseFloat(this.value) / 100);
});
$("audio-hint").onclick = function () {
  audioBlocked = false;
  audioUnlocked = false;
  unlockPreviewAudio();
  pvCmd("enableAudio");
  renderAudioBtn();
};
/* 任意用户手势都尝试解锁一次 iframe 内音轨（自动播放策略）。 */
document.addEventListener("pointerdown", unlockPreviewAudio, true);
document.addEventListener("keydown", unlockPreviewAudio, true);
$("scrub").addEventListener("input", function () {
  pvCmd("goto", parseFloat(this.value) / fps());
});
$("zoom").onchange = fitPreview;
$("bg-mode").onchange = function () {
  $("pvwrap").className = this.value;
  if ($("btn-grid").classList.contains("on")) $("pvwrap").classList.add("grid");
  if ($("btn-safe").classList.contains("on")) $("pvwrap").classList.add("safe");
};
$("btn-grid").onclick = function () {
  this.classList.toggle("on");
  $("pvwrap").classList.toggle("grid", this.classList.contains("on"));
};
$("btn-safe").onclick = function () {
  this.classList.toggle("on");
  $("pvwrap").classList.toggle("safe", this.classList.contains("on"));
};
$("btn-full").onclick = function () {
  if (document.fullscreenElement) { document.exitFullscreen(); return; }
  if ($("pvwrap").requestFullscreen) {
    $("pvwrap").requestFullscreen().then(fitPreview, function () {});
  }
};
/* 时间轴工具栏：与预览传输条复用同一套驱动命令。 */
$("tl-first").onclick = function () { pvCmd("goto", 0); };
$("tl-last").onclick = function () {
  pvCmd("goto", Math.max(0, state.duration - 1 / fps()));
};
$("tl-prevf").onclick = function () { pvCmd("step", -1); };
$("tl-nextf").onclick = function () { pvCmd("step", 1); };
$("tl-play").onclick = function () { pvCmd("toggle"); };
$("tl-fit").onclick = fitTimeline;
$("tl-zoom-in").onclick = function () { zoomTimeline(1.4); };
$("tl-zoom-out").onclick = function () { zoomTimeline(1 / 1.4); };
$("tl-zoom-sel").onclick = function () { zoomToScene(selIdx); };
$("tl-split").onclick = function () { splitAtPlayhead(); };
/* 区间：拖拽框选 / 两端把手 / 区间内播放与循环。 */
$("tl-range-mode").onclick = function () {
  tlRangeMode = !tlRangeMode;
  syncRangeUI();
};
$("tl-range-play").onclick = function () {
  if (!rangeOpen()) return;
  seekTo(tlRange.a);
  pushRange();
  pvCmd("play");
};
$("tl-range-loop").onclick = function () {
  rangeLoop = !rangeLoop;
  syncRangeUI();
  pushRange();
  if (rangeLoop && rangeOpen()) {
    if (lastTime < tlRange.a - 0.02 || lastTime >= tlRange.b - 0.02) {
      seekTo(tlRange.a);
    }
  }
};
$("tl-range-clear").onclick = function () { clearRange(); };
$("tl-snap").onclick = function () {
  tlSnap = !tlSnap;
  this.classList.toggle("on", tlSnap);
  this.textContent = tlSnap ? "吸附 开" : "吸附 关";
};
$("btn-log").onclick = function () {
  var el = $("log");
  el.classList.toggle("open");
  this.classList.toggle("on", el.classList.contains("open"));
};
/* 左栏五面板统一切换（对话/场景/脚本/分镜/源码）：tab 高亮 + 面板互斥。
   对话（chat）是主面板——HF 的 chat 左栏形态。 */
function switchLeftPanel(name) {
  var tabs = { projects: "tab-projects", chat: "tab-chat",
               scenes: "tab-scenes", script: "tab-script",
               storyboard: "tab-storyboard", templates: "tab-templates",
               brand: "tab-brand", source: "tab-source" };
  var panels = { projects: "projects-panel", chat: "chat-panel",
                 scenes: "scenes-panel",
                 script: "script-panel", storyboard: "sb-panel",
                 templates: "templates-panel", brand: "brand-panel",
                 source: "source-panel" };
  for (var k in tabs) {
    $(tabs[k]).classList.toggle("active", k === name);
    $(panels[k]).style.display = k === name ? "flex" : "none";
  }
  if (name === "script") scriptRender();
  if (name === "storyboard") storyboardOpen(true);
  if (name === "templates") tplRender();
  if (name === "brand") brandRender();
  if (name === "projects") projectsRender();
}
var pc = $("pvtool-comment");
if (pc) pc.onclick = function () {
  var ta = $("insp-comment");
  if (ta) {
    ta.scrollIntoView({ block: "center", behavior: "smooth" });
    ta.focus({ preventScroll: true });
    ta.classList.add("insp-flash");
    setTimeout(function () { ta.classList.remove("insp-flash"); }, 900);
  }
};
drawInit();
$("tab-chat").onclick = function () { switchLeftPanel("chat"); };
$("tab-projects").onclick = function () { switchLeftPanel("projects"); };
$("tab-scenes").onclick = function () { switchLeftPanel("scenes"); };
$("tab-script").onclick = function () { switchLeftPanel("script"); };
$("tab-storyboard").onclick = function () {
  switchLeftPanel("storyboard");
};
$("tab-templates").onclick = function () { switchLeftPanel("templates"); };
$("tab-brand").onclick = function () { switchLeftPanel("brand"); };
$("tab-source").onclick = function () { switchLeftPanel("source"); };
/* 预览区 Main/Storyboard 双 tab（HF 编辑器形态）：Main=成片预览，
   Storyboard=全镜网格总览。 */
$("pv-tab-main").onclick = function () {
  $("pv-tab-main").classList.add("active");
  $("pv-tab-sb").classList.remove("active");
  sbGridShow(false);
};
$("pv-tab-sb").onclick = function () {
  $("pv-tab-sb").classList.add("active");
  $("pv-tab-main").classList.remove("active");
  sbGridShow(true);
};
